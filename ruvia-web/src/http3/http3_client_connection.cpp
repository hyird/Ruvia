#include "http3/http3_client_connection.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

#include <asio/post.hpp>

#include "ruvia/core/async.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/web/http_client_types.h"

#include "client/http_client_response_decoding.h"
#include "client/http_client_upload_state.h"

namespace ruvia::detail {
namespace {
using clock_type = std::chrono::steady_clock;
constexpr std::size_t max_buffered_connection_body_bytes = 64 * 1024 * 1024;
// Bounds the RFC 9000 closing period (three PTOs) that follows this client's
// CONNECTION_CLOSE, so a slow or lossy peer cannot hold teardown open.
constexpr clock_type::duration max_closing_period = std::chrono::seconds(1);

// RFC 9114 section 8: the connection error that a protocol failure reports.
std::uint64_t protocol_close_code(const http3_client_sans_io_session_result& result) noexcept {
    return static_cast<std::uint64_t>(result.code_ == http3_connection_error_code::no_error
                                          ? http3_connection_error_code::general_protocol_error
                                          : result.code_);
}

clock_type::duration checked_timeout(std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero() ||
        timeout > std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::duration::max())) {
        throw std::invalid_argument("HTTP/3 connection timeout must be positive and representable");
    }
    return std::chrono::duration_cast<clock_type::duration>(timeout);
}

std::optional<clock_type::duration> checked_timeout(
    std::optional<std::chrono::milliseconds> timeout) {
    return timeout ? std::optional{checked_timeout(*timeout)} : std::nullopt;
}

clock_type::time_point deadline_after(clock_type::time_point now, clock_type::duration timeout) noexcept {
    const auto latest_start = clock_type::time_point::max() - timeout;
    return now > latest_start ? clock_type::time_point::max() : now + timeout;
}

http3_client_sans_io_response_limits response_limits(
    std::size_t max_requests, std::size_t max_response_bytes, http3_qpack_config qpack, bool origins, bool datagrams) {
    if (max_requests == 0 || max_requests > ruvia::quic_limits{}.max_streams_ ||
        max_response_bytes == 0 || max_response_bytes > max_buffered_connection_body_bytes) {
        throw std::invalid_argument("HTTP/3 connection request and body bounds must be positive");
    }
    const auto aggregate = max_response_bytes > std::numeric_limits<std::size_t>::max() / max_requests
                               ? std::numeric_limits<std::size_t>::max()
                               : max_response_bytes * max_requests;
    return {.max_live_streams_ = max_requests,
        .max_body_bytes_per_stream_ = max_response_bytes,
        .max_total_body_bytes_ = std::min(aggregate, max_buffered_connection_body_bytes),
        .connection_ = {.max_active_streams_ = max_requests, .qpack_max_table_capacity_ = qpack.max_table_capacity_, .qpack_blocked_streams_ = qpack.max_blocked_streams_, .enable_datagrams_ = datagrams, .receive_origin_advertisements_ = origins}};
}

std::pmr::memory_resource* require_resource(std::pmr::memory_resource* resource) {
    if (resource == nullptr) {
        throw std::invalid_argument("HTTP/3 connection requires worker-owned storage");
    }
    return resource;
}

http_client_error::code_type client_error_code(http3_client_connection::outcome_type outcome) noexcept {
    using outcome_type = http3_client_connection::outcome_type;
    switch (outcome) {
        case outcome_type::cancelled:
            return http_client_error::code_type::cancelled;
        case outcome_type::deadline:
            return http_client_error::code_type::timeout;
        case outcome_type::connect_failed:
            return http_client_error::code_type::connect_failed;
        case outcome_type::response_too_large:
            return http_client_error::code_type::response_too_large;
        case outcome_type::result_budget_exceeded:
            return http_client_error::code_type::result_budget_exceeded;
        case outcome_type::connection_draining:
        case outcome_type::queue_full:
            return outcome == outcome_type::queue_full ? http_client_error::code_type::queue_full
                                                       : http_client_error::code_type::closing;
        case outcome_type::invalid_request:
            return http_client_error::code_type::invalid_request;
        case outcome_type::protocol_error:
        case outcome_type::request_rejected:
            return http_client_error::code_type::protocol_error;
        case outcome_type::transport_error:
        case outcome_type::pending:
        case outcome_type::complete:
            return http_client_error::code_type::io_error;
    }
    return http_client_error::code_type::io_error;
}

http3_client_connection::outcome_type outcome_for_client_error(http_client_error::code_type error) noexcept {
    switch (error) {
        case http_client_error::code_type::timeout:
            return http3_client_connection::outcome_type::deadline;
        case http_client_error::code_type::cancelled:
            return http3_client_connection::outcome_type::cancelled;
        case http_client_error::code_type::response_too_large:
            return http3_client_connection::outcome_type::response_too_large;
        case http_client_error::code_type::result_budget_exceeded:
            return http3_client_connection::outcome_type::result_budget_exceeded;
        case http_client_error::code_type::protocol_error:
        case http_client_error::code_type::invalid_request:
            return http3_client_connection::outcome_type::protocol_error;
        case http_client_error::code_type::queue_full:
            return http3_client_connection::outcome_type::queue_full;
        case http_client_error::code_type::closing:
            return http3_client_connection::outcome_type::connection_draining;
        case http_client_error::code_type::connect_failed:
            return http3_client_connection::outcome_type::connect_failed;
        case http_client_error::code_type::not_configured:
        case http_client_error::code_type::resolve_failed:
        case http_client_error::code_type::tls_failed:
        case http_client_error::code_type::protocol_unavailable:
        case http_client_error::code_type::io_error:
            return http3_client_connection::outcome_type::transport_error;
    }
    return http3_client_connection::outcome_type::transport_error;
}
}  // namespace

http3_client_connection::http3_client_connection(asio::io_context& io, const worker_handle& worker_value,
    task_scope& pool_tasks, http3_quic_client_tls_context& tls, http_origin_view origin,
    std::chrono::milliseconds connect_timeout, std::pmr::memory_resource* resource,
    std::size_t max_requests, std::size_t max_response_bytes,
    std::chrono::milliseconds idle_timeout, http3_client_body_budget* receive_body_budget,
    std::optional<std::chrono::milliseconds> write_timeout)
    : http3_client_connection(io, worker_value, pool_tasks, tls, origin, connect_timeout, resource,
          max_requests, max_response_bytes, idle_timeout, receive_body_budget, write_timeout, {}, {},
          {}, {}, ruvia::quic_version::v1, false) {}

http3_client_connection::http3_client_connection(asio::io_context& io, const worker_handle& worker_value,
    task_scope& pool_tasks, http3_quic_client_tls_context& tls, http_origin_view origin,
    std::chrono::milliseconds connect_timeout,
    std::pmr::memory_resource* resource, std::size_t max_requests, std::size_t max_response_bytes,
    std::chrono::milliseconds idle_timeout, http3_client_body_budget* receive_body_budget,
    std::optional<std::chrono::milliseconds> write_timeout,
    lifecycle_notification_type lifecycle_notification, http3_qpack_config qpack,
    http3_client_origin_observer origin_observer, http3_client_push_observer push_observer,
    ruvia::quic_version initial_version, bool enable_early_data)
    : owner_thread_(std::this_thread::get_id()),
      io_(io),
      worker_(worker_value),
      pool_tasks_(pool_tasks),
      tls_(tls),
      resource_(require_resource(resource)),
      host_(origin.host(), resource_),
      authority_(make_http_origin_authority(origin, resource_)),
      port_(origin.port()),
      initial_version_(initial_version),
      enable_early_data_(enable_early_data),
      connect_timeout_(checked_timeout(connect_timeout)),
      idle_timeout_(checked_timeout(idle_timeout)),
      write_timeout_(checked_timeout(write_timeout)),
      max_requests_(max_requests),
      max_response_bytes_(max_response_bytes),
      response_limits_(response_limits(max_requests, max_response_bytes, qpack, origin_observer.receive_ != nullptr, true)),
      resolver_(io_, resource_),
      body_budget_(max_buffered_connection_body_bytes),
      receive_body_budget_(receive_body_budget == nullptr ? &body_budget_ : receive_body_budget),
      lifecycle_notification_(lifecycle_notification),
      origin_observer_(origin_observer),
      push_observer_(push_observer),
      receive_body_budget_wake_(*receive_body_budget_, on_receive_body_budget_released, this),
      requests_(resource_),
      pushes_(resource_),
      attempt_(nullptr, pmr_object_deleter<attempt_type>{resource_}) {
    if (!worker_.valid() || origin.scheme() != http_scheme::https || port_ == 0 ||
        (lifecycle_notification_.context_ == nullptr) !=
            (lifecycle_notification_.notify_ == nullptr) ||
        (origin_observer_.context_ == nullptr) != (origin_observer_.receive_ == nullptr)) {
        throw std::invalid_argument(
            "HTTP/3 connection requires a worker, HTTPS origin, and complete notification");
    }
    if ((push_observer_.context_ == nullptr) != (push_observer_.receive_ == nullptr) ||
        (push_observer_.receive_ == nullptr) != (push_observer_.finished_ == nullptr) ||
        (push_observer_.receive_ != nullptr && (!push_observer_.config_.enabled_ || receive_body_budget == nullptr ||
                                                   push_observer_.config_.max_concurrent_pushes_ == 0 ||
                                                   push_observer_.config_.max_concurrent_pushes_ > ruvia::quic_limits{}.max_streams_))) {
        throw std::invalid_argument("HTTP/3 push requires bounded admission and a result-stable body budget");
    }
    if (push_observer_.receive_ != nullptr && push_observer_.config_.timeout_) {
        (void)checked_timeout(*push_observer_.config_.timeout_);
    }
    // URI authority retains IP-literal brackets. DNS/TLS receive only the
    // address, never brackets or the origin's port suffix.
    if (host_.front() == '[') {
        host_.erase(host_.size() - 1);
        host_.erase(0, 1);
    }
}

http3_client_connection::attempt_type::attempt_type(std::pmr::memory_resource* resource,
    http3_client_body_budget& body_budget, const http3_client_sans_io_response_limits& limits)
    : engine_(resource, body_budget, limits),
      receiver_(engine_),
      critical_output_{std::pmr::string(resource), std::pmr::string(resource), std::pmr::string(resource)},
      peer_streams_(resource),
      peer_push_streams_(resource),
      session_(nullptr, pmr_object_deleter<http3_quic_client_socket_session>{resource}) {
    peer_streams_.reserve(ruvia::quic_limits{}.max_streams_);
}

http3_client_connection::~http3_client_connection() {
    if (std::this_thread::get_id() != owner_thread_ || running_ || starting_) {
        std::terminate();
    }
    for (const auto& request : requests_) {
        if (request.response_state_ != nullptr || request.delivery_) {
            // A state contains a typed borrow to this connection. The lifecycle
            // owner must detach it before destroying the connection, even when
            // its body reservation happens to be empty.
            std::terminate();
        }
    }
    if (!pushes_.empty()) {
        std::terminate();
    }
    receive_body_budget_wake_.reset();
    body_budget_.release(retained_result_body_bytes_);
}

void http3_client_connection::require_owner_thread() const {
    if (std::this_thread::get_id() != owner_thread_ || !worker_.is_current()) {
        throw std::logic_error("HTTP/3 client connection must run on its owner worker");
    }
}

void http3_client_connection::on_receive_body_budget_released(void* context_value) noexcept {
    static_cast<http3_client_connection*>(context_value)->wake_receive_driver();
}

void http3_client_connection::on_response_event(void* context_value, const http3_connection_event& event) {
    auto& request = *static_cast<request_type*>(context_value);
    if (request.response_state_ != nullptr) {
        if (auto& upload = request.response_state_->upload_; upload && event.request_content_signal_) {
            if (*event.request_content_signal_ == http_client_request_content_signal::continue_value) {
                upload->content_released_ = true;
                request.continue_deadline_.reset();
                upload->output_.notify_data();
            } else if (!upload->output_.ended_) {
                upload->output_.stop();
                request.continue_deadline_.reset();
            }
        }
        switch (event.kind_) {
            case http3_connection_event_kind::push_stream:
            case http3_connection_event_kind::push_promise:
            case http3_connection_event_kind::push_canceled:
            case http3_connection_event_kind::priority_update:
            case http3_connection_event_kind::origin_advertisement:
                break;
            case http3_connection_event_kind::informational_head:
            case http3_connection_event_kind::final_head:
            case http3_connection_event_kind::body:
            case http3_connection_event_kind::tunnel_data:
            case http3_connection_event_kind::trailer_field:
            case http3_connection_event_kind::message_end:
                request.response_state_->http3_response_started_ = true;
                break;
            case http3_connection_event_kind::request_head:
            case http3_connection_event_kind::reset:
                break;
        }
    }
    if (request.delivery_) {
        const auto sink_value = request.delivery_->event_sink();
        sink_value.callback_(sink_value.context_, event);
    }
}

void http3_client_connection::wake_receive_driver() noexcept {
    if (std::this_thread::get_id() != owner_thread_ || !worker_.is_current()) {
        std::terminate();
    }
    if (auto* session = live_session()) {
        session->notify_work();
    } else if (running_ || starting_) {
        resolver_.interrupt();
    }
}

void http3_client_connection::on_origin_event(void* context_value, const http3_connection_event& event) {
    auto& owner_value = *static_cast<http3_client_connection*>(context_value);
    if (event.origin_advertisement_ != nullptr) {
        owner_value.origin_observer_.receive_(owner_value.origin_observer_.context_,
            owner_value.origin_observer_.connection_slot_, *event.origin_advertisement_);
    }
}

http3_client_connection::attempt_type& http3_client_connection::attempt() {
    if (!attempt_) {
        throw std::logic_error("HTTP/3 protocol state requires an active connection attempt");
    }
    return *attempt_;
}

http3_client_connection::request_list_type::iterator http3_client_connection::find(request_id_type id) noexcept {
    return std::find_if(requests_.begin(), requests_.end(),
        [id](const request_type& request) { return request.id_ == id; });
}

http3_client_connection::request_list_type::const_iterator http3_client_connection::find(request_id_type id) const noexcept {
    return std::find_if(requests_.begin(), requests_.end(),
        [id](const request_type& request) { return request.id_ == id; });
}

http3_client_connection::submission_type http3_client_connection::submit(rejected_request_type request) {
    return submit(std::move(request.request_), request.deadline_);
}

http3_client_connection::submission_type http3_client_connection::submit(
    http_client_request_storage request, std::optional<time_point_type> deadline_value) {
    return submit_impl(std::move(request), deadline_value, nullptr);
}

http3_client_connection::submission_type http3_client_connection::submit(
    http_client_request_storage request, http_client_response_state& response,
    std::optional<time_point_type> deadline_value) {
    return submit_impl(std::move(request), deadline_value, &response);
}

http3_client_connection::submission_type http3_client_connection::submit_impl(
    http_client_request_storage request, std::optional<time_point_type> deadline_value,
    http_client_response_state* response) {
    require_owner_thread();
    if (stopping_ || draining_ || terminal_ ||
        next_request_id_ == std::numeric_limits<request_id_type>::max()) {
        return {.outcome_ = outcome_type::connection_draining};
    }
    if (deadline_value && clock_type::now() >= *deadline_value) {
        return {.outcome_ = outcome_type::deadline};
    }
    if (requests_.size() >= max_requests_) {
        return {.outcome_ = outcome_type::queue_full};
    }
    if (response != nullptr &&
        (response->references_ == 0 || response->http3_connection_ != nullptr || response->complete_ ||
            response->head_ready_ || response->failure_ || response->error_code_ ||
            response->transport_ != http_client_response_transport::unassigned ||
            response->has_http3_body_budget() || !response->buffered_.empty() ||
            !response->pending_.empty())) {
        return {.outcome_ = outcome_type::invalid_request};
    }
    const auto method = classify_http_method(request.method());
    const bool replay_safe = request.replay_safe();
    const bool early_data_eligible = replay_safe && !request.is_tunnel() &&
                                     request.upload() == nullptr && !http_client_request_storage_access::has_body(request) &&
                                     (method == http_known_method::get || method == http_known_method::head);
    auto owned = http3_client_request_write::create(
        std::move(request), "https", authority_, resource_);
    if ((owned.index() != 0)) {
        return {.outcome_ = outcome_type::invalid_request};
    }
    const request_id_type id = ++next_request_id_;
    if (response == nullptr) {
        requests_.emplace_back(id, std::move(std::get<0>(owned)), worker_, resource_, deadline_value, replay_safe);
        requests_.back().early_data_eligible_ = early_data_eligible;
    } else {
        requests_.emplace_back(
            id, std::move(std::get<0>(owned)), worker_, resource_, deadline_value, replay_safe, *response,
            *receive_body_budget_);
        requests_.back().early_data_eligible_ = early_data_eligible;
        response->retain_reference();
        response->request_method_ = method;
        response->transport_ = http_client_response_transport::http3;
        response->http3_connection_ = this;
        response->http3_request_id_ = id;
        response->request_id_ = id;
        response->buffered_limit_ = std::min(response->buffered_limit_, max_response_bytes_);
        if (auto* output = response->output()) {
            output->wake_target_ = this;
            output->wake_ = [](void* target) noexcept { static_cast<http3_client_connection*>(target)->wake_receive_driver(); };
        }
    }
    if (auto* session = live_session()) {
        session->notify_work();
    } else {
        resolver_.interrupt();  // Re-arm DNS to include a newly earlier deadline.
    }
    return {.id_ = id};
}

void http3_client_connection::start() {
    require_owner_thread();
    const bool has_pending = std::any_of(requests_.begin(), requests_.end(),
        [](const request_type& request) {
            return request.response_.outcome_ == outcome_type::pending;
        });
    if (starting_ || running_ || stopping_ || terminal_ || !has_pending) {
        throw std::logic_error("HTTP/3 connection cannot start without a live request");
    }
    starting_ = true;
    try {
        pool_tasks_.spawn(drive());
    } catch (...) {
        starting_ = false;
        throw;
    }
}

void http3_client_connection::start_if_needed() {
    require_owner_thread();
    if (!running_ && !starting_) {
        start();
    }
}

bool http3_client_connection::reprioritize(request_id_type id, http_priority priority) {
    require_owner_thread();
    if (auto push = find_push(id); push != pushes_.end()) {
        if (stopping_ || terminal_ || push->cancel_requested_ || !attempt_ ||
            !attempt_->engine_.queue_push_priority_update(push->push_id_, priority)) {
            return false;
        }
        wake_receive_driver();
        return true;
    }
    const auto request = find(id);
    if (stopping_ || terminal_ || request == requests_.end() ||
        request->response_.outcome_ != outcome_type::pending || !request->writer_.stream_id()) {
        return false;
    }
    // A 0-RTT stream defers the update until its attempt's handshake completes.
    if (auto* session = live_session(); !attempt_ || (session != nullptr &&
                                                         session->transport().info().state_ !=
                                                             ruvia::quic_connection_state::ready)) {
        request->pending_priority_update_ = priority;
        wake_receive_driver();
        return true;
    }
    if (!attempt_->engine_.queue_priority_update(*request->writer_.stream_id(), priority)) {
        return false;
    }
    wake_receive_driver();
    return true;
}

void http3_client_connection::cancel(request_id_type id) noexcept {
    if (std::this_thread::get_id() != owner_thread_ || !worker_.is_current()) {
        std::terminate();
    }
    if (auto push = find_push(id); push != pushes_.end()) {
        push->cancel_requested_ = true;
        wake_receive_driver();
        return;
    }
    const auto found = find(id);
    if (found == requests_.end() || found->response_.outcome_ != outcome_type::pending) {
        return;
    }
    if (!found->writer_.stream_id()) {
        finish_request(*found, outcome_type::cancelled);
        if (auto* session = live_session()) {
            session->notify_work();
        } else {
            resolver_.interrupt();
        }
        return;
    }
    found->cancel_requested_ = true;
    if (auto* session = live_session()) {
        session->notify_work();
    }
}

task<void> http3_client_connection::wait(request_id_type id) {
    require_owner_thread();
    const auto found = find(id);
    if (found == requests_.end()) {
        throw std::invalid_argument("HTTP/3 request ID not found");
    }
    auto& request = *found;
    ++request.waiters_;
    struct wait_guard final {
        http3_client_connection& connection_;
        request_type& request_;
        ~wait_guard() {
            --request_.waiters_;
            connection_.maybe_release_response_request(request_);
        }
    } guard_value{*this, request};
    while (request.response_.outcome_ == outcome_type::pending) {
        co_await request.signal_.wait();
    }
}

const http3_client_connection::response_type* http3_client_connection::result(request_id_type id) const noexcept {
    const auto found = find(id);
    return found == requests_.end() || found->response_.outcome_ == outcome_type::pending
               ? nullptr
               : &found->response_;
}

std::optional<http3_client_connection::rejected_request_type> http3_client_connection::take_rejected_request(request_id_type id) {
    require_owner_thread();
    const auto found = find(id);
    if (found == requests_.end() || found->response_.outcome_ != outcome_type::request_rejected ||
        found->waiters_ != 0 || !found->stream_retired_) {
        return std::nullopt;
    }
    auto& request = *found;
    auto* const state_value = request.response_state_;
    if ((state_value == nullptr) != !request.delivery_.has_value()) {
        std::terminate();
    }
    if (state_value != nullptr &&
        (state_value->http3_connection_ != this || state_value->http3_request_id_ != request.id_ ||
            state_value->transport_ != http_client_response_transport::http3 || state_value->abandoned_ ||
            request.consumer_released_ || state_value->http3_response_started_ || state_value->head_ready_ ||
            state_value->complete_ ||
            state_value->failure_ || state_value->error_code_ ||
            state_value->producer_body_bytes() != 0 || !state_value->has_http3_body_budget() ||
            state_value->http3_body_budget_.retained_bytes() != 0)) {
        return std::nullopt;
    }

    auto owned_request = request.writer_.take_request_after_retirement();
    if (!owned_request) {
        return std::nullopt;
    }
    rejected_request_type handoff{.request_ = std::move(*owned_request), .deadline_ = request.deadline_};
    if (state_value != nullptr) {
        state_value->release_http3_body_budget();
        state_value->http3_connection_ = nullptr;
        state_value->http3_request_id_ = 0;
        state_value->request_id_ = 0;
        state_value->cancellation_id_ = 0;
        state_value->transport_ = http_client_response_transport::unassigned;
        request.response_state_ = nullptr;
        request.delivery_.reset();
        state_value->release_reference();
    }
    requests_.erase(found);
    return handoff;
}

bool http3_client_connection::release(request_id_type id) noexcept {
    if (std::this_thread::get_id() != owner_thread_ || !worker_.is_current()) {
        std::terminate();
    }
    const auto found = find(id);
    if (found == requests_.end() || found->response_.outcome_ == outcome_type::pending ||
        found->waiters_ != 0 || found->response_state_ != nullptr) {
        return false;
    }
    if (found->response_.outcome_ == outcome_type::complete) {
        body_budget_.release(found->response_.body_.size());
        retained_result_body_bytes_ -= found->response_.body_.size();
        if (auto* session = live_session()) {
            session->notify_work();
        }
    }
    requests_.erase(found);
    return true;
}

bool http3_client_connection::release_response_request(request_id_type id) noexcept {
    if (std::this_thread::get_id() != owner_thread_ || !worker_.is_current()) {
        std::terminate();
    }
    const auto found = find(id);
    if (found == requests_.end() || found->response_.outcome_ == outcome_type::pending ||
        found->response_.outcome_ == outcome_type::request_rejected || !found->stream_retired_ ||
        found->waiters_ != 0 || found->response_state_ == nullptr || !found->delivery_) {
        return false;
    }

    auto& request = *found;
    auto* const state_value = request.response_state_;
    if (state_value->http3_connection_ != this || state_value->http3_request_id_ != request.id_ ||
        state_value->transport_ != http_client_response_transport::http3) {
        std::terminate();
    }
    const auto body_bytes = state_value->producer_body_bytes();
    if (request.delivery_->retained_body_bytes() != body_bytes ||
        state_value->http3_body_budget_.retained_bytes() != body_bytes) {
        return false;
    }
    if (receive_body_budget_ == &body_budget_ && body_bytes != 0) {
        return false;
    }
    if (body_bytes == 0) {
        state_value->release_http3_body_budget();
    }

    state_value->http3_connection_ = nullptr;
    state_value->http3_request_id_ = 0;
    state_value->request_id_ = 0;
    state_value->transport_ = http_client_response_transport::unassigned;
    request.response_state_ = nullptr;
    request.delivery_.reset();
    state_value->release_reference();
    requests_.erase(found);
    return true;
}

void http3_client_connection::maybe_release_response_request(request_type& request) noexcept {
    if (!request.consumer_released_ || request.waiters_ != 0 ||
        request.response_.outcome_ == outcome_type::pending || !request.stream_retired_) {
        return;
    }
    auto* const state_value = request.response_state_;
    if (state_value == nullptr || !request.delivery_) {
        std::terminate();
    }
    state_value->discard_response_body();
    if (request.response_.outcome_ == outcome_type::request_rejected) {
        state_value->release_http3_body_budget();
        state_value->http3_connection_ = nullptr;
        state_value->http3_request_id_ = 0;
        state_value->request_id_ = 0;
        state_value->transport_ = http_client_response_transport::unassigned;
        request.response_state_ = nullptr;
        request.delivery_.reset();
        state_value->release_reference();
        const auto found = find(request.id_);
        if (found == requests_.end()) {
            std::terminate();
        }
        requests_.erase(found);
        return;
    }
    if (!release_response_request(request.id_)) {
        std::terminate();
    }
}

void http3_client_connection::reap_released_response_requests() noexcept {
    for (auto it = requests_.begin(); it != requests_.end();) {
        auto current = it++;
        auto& request = *current;
        if (!request.consumer_released_ || request.waiters_ != 0 ||
            request.response_.outcome_ == outcome_type::pending || !request.stream_retired_) {
            continue;
        }
        maybe_release_response_request(request);
    }
}

void http3_client_connection::abandon_response(request_id_type id) noexcept {
    if (std::this_thread::get_id() != owner_thread_ || !worker_.is_current()) {
        std::terminate();
    }
    if (auto push = find_push(id); push != pushes_.end()) {
        push->cancel_requested_ = true;
        wake_receive_driver();
        return;
    }
    const auto found = find(id);
    if (found == requests_.end() || !found->delivery_ || found->response_state_ == nullptr) {
        return;
    }
    auto& request = *found;
    request.response_state_->abandoned_ = true;
    if (request.response_.outcome_ != outcome_type::pending) {
        return;
    }
    if (!request.writer_.stream_id()) {
        finish_request(request, outcome_type::cancelled);
    } else {
        request.cancel_requested_ = true;
        if (auto* session = live_session()) {
            session->notify_work();
        }
    }
}

void http3_client_connection::consumer_released(request_id_type id) noexcept {
    if (std::this_thread::get_id() != owner_thread_ || !worker_.is_current()) {
        std::terminate();
    }
    if (auto push = find_push(id); push != pushes_.end()) {
        push->cancel_requested_ = true;
        wake_receive_driver();
        return;
    }
    const auto found = find(id);
    if (found == requests_.end() || !found->delivery_ || found->response_state_ == nullptr) {
        return;
    }
    auto& request = *found;
    request.consumer_released_ = true;
    if (request.response_.outcome_ == outcome_type::pending) {
        if (!request.writer_.stream_id()) {
            finish_request(request, outcome_type::cancelled);
            maybe_release_response_request(request);
            return;
        }
        request.cancel_requested_ = true;
        if (auto* session = live_session()) {
            session->notify_work();
        }
    }
    maybe_release_response_request(request);
}

ruvia::quic_path_migration http3_client_connection::start_path_migration(
    const asio::ip::udp::endpoint& local_endpoint) {
    require_owner_thread();
    auto* session = live_session();
    if (stopping_ || session == nullptr || !running_) {
        return {.status_ = ruvia::quic_migration_status::rejected};
    }
    auto migration = session->start_path_migration(local_endpoint);
    if (migration.status_ == ruvia::quic_migration_status::started ||
        migration.status_ == ruvia::quic_migration_status::validated) {
        quic_connection_migration_id_ = migration.id_;
        quic_path_migration_generation_ = quic_generation_;
        auto id = next_path_migration_id_++;
        if (id == 0) {
            id = next_path_migration_id_++;
        }
        migration.id_ = id;
        quic_path_migration_ = migration;
    }
    return migration;
}

std::optional<ruvia::quic_path_migration> http3_client_connection::path_migration(
    std::uint64_t id) const noexcept {
    if (!quic_path_migration_ || quic_path_migration_->id_ != id) {
        return std::nullopt;
    }
    if (const auto* session = live_session(); session != nullptr && quic_path_migration_generation_ == quic_generation_) {
        if (const auto migration = session->path_migration(quic_connection_migration_id_)) {
            auto result_value = *migration;
            result_value.id_ = id;
            return result_value;
        }
    }
    return quic_path_migration_;
}

std::optional<ruvia::quic_path_migration> http3_client_connection::active_path_migration() const noexcept {
    if (const auto* session = live_session(); quic_path_migration_ && session != nullptr &&
                                              quic_path_migration_generation_ == quic_generation_) {
        if (const auto migration = session->path_migration(quic_connection_migration_id_)) {
            auto result_value = *migration;
            result_value.id_ = quic_path_migration_->id_;
            return result_value;
        }
    }
    return quic_path_migration_;
}

ruvia::quic_operation_status http3_client_connection::cancel_path_migration(std::uint64_t id) {
    require_owner_thread();
    auto* session = live_session();
    if (session == nullptr || !quic_path_migration_ || quic_path_migration_->id_ != id) {
        return ruvia::quic_operation_status::retired;
    }
    const auto status = session->cancel_path_migration(quic_connection_migration_id_);
    if (status == ruvia::quic_operation_status::accepted) {
        request_stop();
    }
    return status;
}

void http3_client_connection::cache_path_migration() noexcept {
    const auto* session = live_session();
    if (session == nullptr || !quic_path_migration_ ||
        quic_path_migration_generation_ != quic_generation_) {
        return;
    }
    if (const auto migration = session->path_migration(quic_connection_migration_id_)) {
        quic_path_migration_->status_ = migration->status_;
        quic_path_migration_->local_address_ = migration->local_address_;
    }
    if (quic_path_migration_->status_ == ruvia::quic_migration_status::started) {
        quic_path_migration_->status_ = ruvia::quic_migration_status::aborted;
    }
}

void http3_client_connection::request_stop() noexcept {
    if (std::this_thread::get_id() != owner_thread_ || !worker_.is_current()) {
        std::terminate();
    }
    stopping_ = true;
    resolver_.request_stop();
    if (auto* session = live_session()) {
        session->request_stop();
    }
    if (!running_ && !starting_) {
        finish_all(outcome_type::cancelled);
    }
}

std::optional<http3_client_connection::time_point_type> http3_client_connection::next_deadline(
    std::optional<time_point_type> connect_deadline) const noexcept {
    for (const auto& request : requests_) {
        if (request.response_.outcome_ != outcome_type::pending) {
            continue;
        }
        if (request.deadline_ && (!connect_deadline || *request.deadline_ < *connect_deadline)) {
            connect_deadline = request.deadline_;
        }
        if (request.continue_deadline_ && (!connect_deadline || *request.continue_deadline_ < *connect_deadline)) {
            connect_deadline = request.continue_deadline_;
        }
        if (request.write_deadline_ &&
            (!connect_deadline || *request.write_deadline_ < *connect_deadline)) {
            connect_deadline = request.write_deadline_;
        }
    }
    for (const auto& push : pushes_) {
        if (push.deadline_ && (!connect_deadline || *push.deadline_ < *connect_deadline)) {
            connect_deadline = push.deadline_;
        }
    }
    if (attempt_) {
        for (const auto& stream : attempt_->peer_push_streams_) {
            if (stream.deadline_ && std::none_of(pushes_.begin(), pushes_.end(), [&stream](const push_type& push) { return push.stream_id_ == stream.stream_id_; }) &&
                (!connect_deadline || *stream.deadline_ < *connect_deadline)) {
                connect_deadline = stream.deadline_;
            }
        }
        for (const auto& candidate : attempt_->critical_write_deadlines_) {
            if (candidate && (!connect_deadline || *candidate < *connect_deadline)) {
                connect_deadline = candidate;
            }
        }
    }
    return connect_deadline;
}

bool http3_client_connection::retire_request(request_type& request, bool graceful) noexcept {
    if (request.stream_retired_) {
        return true;
    }
    const auto id = request.writer_.stream_id();
    if (!id) {
        request.stream_retired_ = true;
        return true;
    }
    if (!attempt_) {
        // The driver already retired the whole attempt: no peer input can
        // reach this stream or the parser state that the attempt owned.
        request.response_parser_registered_ = false;
        request.stream_retired_ = true;
        return true;
    }
    auto& attempt = *attempt_;
    try {
        if (auto* session = attempt.session_.get()) {
            // Only a FIN-completed send half can retire gracefully. A send half
            // closed by local stop or peer STOP_SENDING is never send-complete.
            const bool graceful_tunnel = graceful && request.writer_.finished() && request.response_state_ != nullptr &&
                                         request.response_state_->tunnel_ && request.response_state_->tunnel_->accepted_ &&
                                         request.response_state_->tunnel_->receive_ended_ &&
                                         request.response_state_->tunnel_->output_.ended_;
            const auto closed = graceful_tunnel
                                    ? session->transport().retire_completed_stream(*id)
                                    : session->transport().terminate_bidirectional_stream(
                                          *id, static_cast<std::uint64_t>(http3_connection_error_code::request_cancelled));
            if (closed != ruvia::quic_operation_status::accepted &&
                closed != ruvia::quic_operation_status::completed &&
                closed != ruvia::quic_operation_status::retired) {
                // Leave parser storage intact until the driver closes the
                // entire session. Local retirement cannot stop QUIC delivery.
                return false;
            }
        }
        // No session means the sole driver has already destroyed transport.
        if (!request.response_parser_registered_) {
            // The QUIC stream has now been physically retired, but registration
            // never installed parser state (for example, allocation failed).
            request.stream_retired_ = true;
            return true;
        }
        auto parsed_value = attempt.engine_.response(*id);
        if (!parsed_value) {
            (void)attempt.engine_.cancel_request(*id);
            parsed_value = attempt.engine_.response(*id);
        }
        if (!parsed_value) {
            if (attempt.session_) {
                return false;
            }
            // The whole QUIC session has already been destroyed; no peer input
            // can reach this now-absent parser node or its former sink.
            request.response_parser_registered_ = false;
            request.stream_retired_ = true;
            return true;
        }
        if (parsed_value->response_body_plan_) {
            request.response_.response_body_plan_ = parsed_value->response_body_plan_;
        }
        attempt.receiver_.retire(*id);
        request.stream_retired_ = attempt.engine_.release(*id);
        if (request.stream_retired_) {
            request.response_parser_registered_ = false;
        }
        return request.stream_retired_;
    } catch (...) {
        return false;
    }
}

void http3_client_connection::finish_request(request_type& request, outcome_type outcome) {
    if (request.response_.outcome_ != outcome_type::pending) {
        return;
    }
    request.write_deadline_.reset();
    request.continue_deadline_.reset();
    auto* state_value = request.response_state_;
    auto* delivery = request.delivery_ ? &*request.delivery_ : nullptr;
    if (outcome == outcome_type::complete) {
        const auto id = request.writer_.stream_id();
        auto& engine = attempt().engine_;
        const auto parsed_value = id ? engine.response(*id) : std::nullopt;
        if (!parsed_value || !parsed_value->complete_) {
            throw std::logic_error("HTTP/3 response completion without valid FIN");
        }
        if (parsed_value->body_.size() > max_buffered_connection_body_bytes - retained_result_body_bytes_) {
            outcome = outcome_type::response_too_large;
        } else {
            request.response_.status_ = parsed_value->status_;
            request.response_.response_body_plan_ = parsed_value->response_body_plan_;
            if (delivery != nullptr) {
                if (state_value == nullptr || !state_value->head_ready_ || !delivery->response_body_plan()) {
                    throw std::logic_error("HTTP/3 incremental response completed without its head");
                }
                // The sink already copied headers and trailers directly into
                // the stable response state. Do not transfer or copy them again.
            } else {
                request.response_.headers_.reserve(parsed_value->headers_.size());
                for (const auto& field : parsed_value->headers_) {
                    request.response_.headers_.push_back(
                        http_header::copy_of(field.name_, field.value_, resource_));
                }
                request.response_.trailers_.reserve(parsed_value->trailers_.size());
                for (const auto& field : parsed_value->trailers_) {
                    request.response_.trailers_.push_back(
                        http_header::copy_of(field.name_, field.value_, resource_));
                }
                auto body = engine.take_body(*id);
                if (!body) {
                    throw std::logic_error("HTTP/3 completed body has already been transferred");
                }
                request.response_.body_ = std::move(*body);
            }
        }
    }

    if (!retire_request(request, outcome == outcome_type::complete) && live_session() != nullptr) {
        if (outcome == outcome_type::complete && state_value != nullptr && state_value->tunnel_ && state_value->tunnel_->accepted_) {
            // Queuing END_STREAM is not physical FIN submission. Keep driving
            // queued tunnel output before releasing the stream/parser owner.
            return;
        }
        // The only driver owns all QUIC callback storage. If STOP_SENDING or
        // parser retirement failed, close/join that driver before publishing a
        // terminal response or releasing the delivery sink.
        throw std::runtime_error("HTTP/3 request stream could not be retired locally");
    }

    if (state_value != nullptr) {
        if (auto* output = state_value->output()) {
            output->wake_ = nullptr;
            output->wake_target_ = nullptr;
            output->stop();
        }
    }
    const bool handoff_rejected = delivery != nullptr && outcome == outcome_type::request_rejected &&
                                  terminal_failure_ != outcome_type::protocol_error && state_value != nullptr &&
                                  state_value->output() == nullptr && !state_value->http3_response_started_ && !state_value->head_ready_ &&
                                  !state_value->complete_ && !state_value->failure_ &&
                                  !state_value->error_code_ && state_value->producer_body_bytes() == 0 &&
                                  delivery->callback_failure() == nullptr &&
                                  delivery->retirement_reason() ==
                                      http3_client_response_delivery::retirement_reason_type::none;
    if (delivery != nullptr) {
        if (handoff_rejected) {
            // Keep the public response pending until the pool retries this
            // stream on a fresh connection or commits the rejection to it.
        } else if (outcome == outcome_type::request_rejected) {
            outcome = outcome_type::protocol_error;
            (void)delivery->commit_terminal_error(http_client_error::code_type::protocol_error);
        } else if (terminal_failure_ == outcome_type::protocol_error) {
            outcome = outcome_type::protocol_error;
            (void)delivery->commit_terminal_error(http_client_error::code_type::protocol_error);
        } else if (delivery->callback_failure() != nullptr) {
            outcome = outcome_type::protocol_error;
            (void)delivery->commit_failure(delivery->callback_failure());
        } else if (delivery->retirement_reason() !=
                   http3_client_response_delivery::retirement_reason_type::none) {
            switch (delivery->retirement_reason()) {
                case http3_client_response_delivery::retirement_reason_type::response_too_large:
                    outcome = outcome_type::response_too_large;
                    (void)delivery->commit_retirement_failure(
                        http_client_error::code_type::response_too_large);
                    break;
                case http3_client_response_delivery::retirement_reason_type::protocol_error:
                    outcome = outcome_type::protocol_error;
                    (void)delivery->commit_retirement_failure(
                        http_client_error::code_type::protocol_error);
                    break;
                case http3_client_response_delivery::retirement_reason_type::callback_failure:
                    outcome = outcome_type::protocol_error;
                    (void)delivery->commit_failure(delivery->callback_failure());
                    break;
                case http3_client_response_delivery::retirement_reason_type::none:
                    std::terminate();
            }
        } else if (outcome == outcome_type::complete) {
            try {
                if (request.deadline_ && clock_type::now() >= *request.deadline_) {
                    outcome = outcome_type::deadline;
                } else {
                    const auto plan = delivery->response_body_plan();
                    const bool content_semantics_present =
                        plan && plan->content_semantics() == http_response_content_semantics_type::with_content;
                    // Only a decode-required body is withheld from incremental
                    // readers; a streamed body may be borrowed across a pipe write.
                    const bool body_withheld = state_value->body_decode_required_;
                    decode_http_client_response_content_encoding(
                        *state_value, content_semantics_present, max_response_bytes_);
                    if (request.deadline_ && clock_type::now() >= *request.deadline_) {
                        outcome = outcome_type::deadline;
                        if (body_withheld) {
                            state_value->discard_response_body();
                        }
                    }
                }
                if (outcome == outcome_type::complete) {
                    const auto committed = delivery->commit_complete();
                    if (committed != http3_client_response_delivery::commit_status_type::committed) {
                        outcome = outcome_type::protocol_error;
                        (void)delivery->commit_terminal_error(
                            http_client_error::code_type::protocol_error);
                    }
                } else {
                    (void)delivery->commit_terminal_error(client_error_code(outcome));
                }
            } catch (...) {
                const auto failure = std::current_exception();
                try {
                    std::rethrow_exception(failure);
                } catch (const http_client_error& error) {
                    outcome = outcome_for_client_error(error.code());
                } catch (...) {
                    outcome = outcome_type::protocol_error;
                }
                (void)delivery->commit_failure(failure);
            }
        } else {
            (void)delivery->commit_terminal_error(client_error_code(outcome));
        }
    } else if (outcome == outcome_type::complete) {
        // take_body() relinquished the receive owner's reservation. There is
        // no suspension or further receive between that transfer and this
        // result-owner reservation on the same worker.
        if (!body_budget_.try_retain(request.response_.body_.size())) {
            throw std::logic_error("HTTP/3 transferred body lost its budget reservation");
        }
        retained_result_body_bytes_ += request.response_.body_.size();
    } else {
        std::pmr::string empty(resource_);
        request.response_.body_.swap(empty);
    }
    request.response_.outcome_ = outcome;
    request.signal_.notify();
}

void http3_client_connection::finish_all(outcome_type outcome) noexcept {
    for (auto& request : requests_) {
        if (request.response_.outcome_ == outcome_type::pending) {
            try {
                finish_request(request, outcome);
            } catch (...) {
                // The driver has already closed QUIC before connection-wide
                // terminal publication. Preserve the failure for any bound
                // response state and never strand a waiter.
                const auto failure = std::current_exception();
                if (!retire_request(request)) {
                    std::terminate();
                }
                if (request.delivery_ && !request.delivery_->commit_failure(failure)) {
                    std::terminate();
                }
                request.response_.outcome_ = outcome_type::transport_error;
                request.signal_.notify();
            }
        }
    }
    reap_released_response_requests();
}

task<void> http3_client_connection::drive() {
    running_ = true;
    starting_ = false;
    outcome_type failure = outcome_type::connect_failed;
    try {
        const auto connect_deadline = deadline_after(clock_type::now(), connect_timeout_);
        std::optional<http3_quic_client_endpoint_resolver::result_type> resolved;
        while (!stopping_) {
            auto attempt_value = co_await resolver_.resolve(host_, port_, next_deadline(connect_deadline));
            const auto now = clock_type::now();
            for (auto& request : requests_) {
                if (request.response_.outcome_ == outcome_type::pending && request.deadline_ &&
                    now >= *request.deadline_) {
                    finish_request(request, outcome_type::deadline);
                }
            }
            const bool still_pending = std::any_of(requests_.begin(), requests_.end(),
                [](const request_type& request) {
                    return request.response_.outcome_ == outcome_type::pending;
                });
            if (!still_pending) {
                break;
            }
            if (now >= connect_deadline ||
                (attempt_value.status_ != http3_quic_client_endpoint_resolver::status_type::interrupted &&
                    attempt_value.status_ != http3_quic_client_endpoint_resolver::status_type::timeout)) {
                resolved.emplace(std::move(attempt_value));
                break;
            }
        }
        if (stopping_ || (resolved && resolved->status_ == http3_quic_client_endpoint_resolver::status_type::stopped)) {
            failure = outcome_type::cancelled;
        } else if (!resolved || resolved->status_ == http3_quic_client_endpoint_resolver::status_type::timeout ||
                   clock_type::now() >= connect_deadline) {
            failure = outcome_type::deadline;
        } else if (resolved->status_ == http3_quic_client_endpoint_resolver::status_type::resolved) {
            for (std::size_t index = 0; index < resolved->endpoints_.size(); ++index) {
                const auto now = clock_type::now();
                if (stopping_ || now >= connect_deadline) {
                    failure = stopping_ ? outcome_type::cancelled : outcome_type::deadline;
                    break;
                }
                const auto remaining_addresses = resolved->endpoints_.size() - index;
                const auto share = clock_type::duration(
                    (connect_deadline - now).count() /
                    static_cast<clock_type::duration::rep>(remaining_addresses));
                const auto attempt_budget = std::min(connect_deadline - now,
                    std::max(share, std::chrono::duration_cast<clock_type::duration>(
                                        std::chrono::milliseconds(100))));
                const bool try_next = co_await drive_endpoint(
                    resolved->endpoints_[index], now + attempt_budget);
                if (!try_next || stopping_) {
                    failure = stopping_ ? outcome_type::cancelled : outcome_type::transport_error;
                    break;
                }
                const bool still_pending = std::any_of(requests_.begin(), requests_.end(),
                    [](const request_type& request) {
                        return request.response_.outcome_ == outcome_type::pending;
                    });
                if (!still_pending || index + 1 == resolved->endpoints_.size()) {
                    break;
                }
                // This attempt never completed its handshake. Close it within
                // the connect budget, then let the next endpoint start from
                // fresh protocol state with re-armed requests.
                co_await close_attempt(std::min(connect_deadline, deadline_after(clock_type::now(), max_closing_period)));
                rearm_attempt_requests();
                attempt_.reset();
            }
            if (!stopping_ && clock_type::now() >= connect_deadline &&
                failure == outcome_type::connect_failed) {
                failure = outcome_type::deadline;
            }
        }
    } catch (...) {
        failure = stopping_ ? outcome_type::cancelled : terminal_failure_;
        if (!close_error_code_ && !stopping_ && terminal_failure_ != outcome_type::deadline) {
            close_error_code_ = static_cast<std::uint64_t>(http3_connection_error_code::internal_error);
        }
    }
    // Admission ends before the closing period can suspend this driver, so a
    // late submission waits for the next connection instead of this outcome.
    draining_ = true;
    if (attempt_) {
        try {
            co_await close_attempt(deadline_after(clock_type::now(), max_closing_period));
        } catch (...) {
            // The session destructor still releases transport and sockets.
            attempt_->session_.reset();
        }
        // All QUIC delivery is closed before parser storage retires or any
        // terminal response waiter is notified.
        (void)attempt_->engine_.stop();
    }
    while (!pushes_.empty()) {
        finish_push(pushes_.begin(), failure);
    }
    finish_all(failure);
    attempt_.reset();
    terminal_ = true;
    running_ = false;
    if (lifecycle_notification_.notify_ != nullptr) {
        lifecycle_notification_.notify_(lifecycle_notification_.context_);
    }
}

void http3_client_connection::begin_attempt(const asio::ip::udp::endpoint& peer) {
    attempt_ = make_pmr_object<attempt_type>(resource_, resource_, body_budget_, response_limits_);
    auto& attempt = *attempt_;
    if (push_observer_.receive_ != nullptr) {
        attempt.peer_push_streams_.reserve(ruvia::quic_limits{}.max_streams_);
        attempt.authorized_push_id_ = push_observer_.config_.max_concurrent_pushes_ - 1;
        if (!attempt.engine_.queue_max_push_id(attempt.authorized_push_id_)) {
            throw std::logic_error("HTTP/3 initial push authorization failed");
        }
        attempt.engine_.observe_pushes(on_push_event, this);
    }
    if (origin_observer_.receive_ != nullptr) {
        attempt.engine_.observe_origins(on_origin_event, this);
    }
    if (++quic_generation_ == 0) {
        ++quic_generation_;
    }
    // GOAWAY and closing state belonged to the previous QUIC connection.
    draining_ = false;
    attempt.session_ = make_pmr_object<http3_quic_client_socket_session>(resource_, io_, peer, host_, tls_,
        attempt.engine_.local_settings(), initial_version_, enable_early_data_, resource_);
}

task<void> http3_client_connection::close_attempt(time_point_type latest) {
    auto& attempt = *attempt_;
    if (!attempt.session_) {
        co_return;
    }
    cache_path_migration();
    static constexpr std::string_view reason = "HTTP/3 connection closed";
    co_await attempt.session_->shutdown({.kind_ = ruvia::quic_close_kind::application,
                                            .code_ = close_error_code_.value_or(static_cast<std::uint64_t>(http3_connection_error_code::no_error)),
                                            .reason_ = {reason.data(), reason.size()}},
        latest);
    attempt.session_.reset();
}

void http3_client_connection::rearm_attempt_requests() {
    // Only replay-safe 0-RTT requests can own a stream of an attempt that
    // never completed its handshake; pushes need a completed handshake.
    while (!pushes_.empty()) {
        finish_push(pushes_.begin(), outcome_type::transport_error);
    }
    for (auto& request : requests_) {
        if (request.response_.outcome_ != outcome_type::pending) {
            continue;
        }
        request.write_deadline_.reset();
        request.continue_deadline_.reset();
        if (request.writer_.stream_id()) {
            rearm_early_request(request);
        }
    }
}

// The request's stream can no longer deliver input: its attempt closed, or
// the peer rejected 0-RTT. Retire its parser state and re-arm the writer so a
// fresh stream repeats the request from its start: as 0-RTT again only when
// the next handshake resumes with early data, otherwise after it completes.
void http3_client_connection::rearm_early_request(request_type& request) {
    auto& attempt = this->attempt();
    const auto stream_id = *request.writer_.stream_id();
    const bool response_started = request.response_state_ != nullptr &&
                                  request.response_state_->http3_response_started_;
    if (!request.early_data_eligible_ || response_started ||
        !request.response_parser_registered_ || !attempt.engine_.cancel_request(stream_id)) {
        throw std::runtime_error("HTTP/3 cannot replay an ineligible or committed 0-RTT request");
    }
    attempt.receiver_.retire(stream_id);
    if (!attempt.engine_.release(stream_id)) {
        throw std::logic_error("HTTP/3 replayed 0-RTT response state could not be retired");
    }
    request.response_parser_registered_ = false;
    request.stream_retired_ = true;
    request.write_deadline_.reset();
    if (request.cancel_requested_) {
        finish_request(request, outcome_type::cancelled);
        return;
    }
    if (!request.writer_.replay_on_new_stream("https", authority_, resource_)) {
        finish_request(request, outcome_type::transport_error);
        return;
    }
    request.stream_retired_ = false;
}

void http3_client_connection::fail_connection(outcome_type outcome, std::uint64_t close_code, const char* message) {
    terminal_failure_ = outcome;
    close_error_code_ = close_code;
    throw std::runtime_error(message);
}

task<bool> http3_client_connection::drive_endpoint(
    const asio::ip::udp::endpoint& peer, time_point_type connect_deadline) {
    begin_attempt(peer);
    auto& attempt = *attempt_;
    auto& session = *attempt.session_;
    bool had_http3 = false;
    std::optional<time_point_type> idle_deadline;
    std::size_t consecutive_work_ticks = 0;
    while (!stopping_) {
        (void)session.consume_work_notification();
        const auto tick = session.pump();
        const bool rejected_early_streams = replay_rejected_early_streams();
        if (tick.status_ == http3_quic_client_socket_session::pump_status_type::fatal ||
            tick.status_ == http3_quic_client_socket_session::pump_status_type::closed) {
            break;
        }
        const bool ready = session.transport().info().state_ ==
                           ruvia::quic_connection_state::ready;
        had_http3 |= ready;
        const auto now = clock_type::now();
        for (auto& request : requests_) {
            if (!ready && request.response_.outcome_ == outcome_type::pending && request.deadline_ &&
                now >= *request.deadline_) {
                finish_request(request, outcome_type::deadline);
            }
        }
        if (now >= connect_deadline && !ready) {
            break;
        }
        const bool early = !ready && session.early_data_enabled() &&
                           session.transport().info().early_data_ ==
                               ruvia::quic_early_data_state::available;
        const bool protocol_progress = rejected_early_streams ||
                                       ((ready || early) && sweep(tick.critical_streams_ready_, early));
        if (attempt.engine_.peer_settings()) {
            session.remember_resumption_ticket(attempt.engine_.peer_settings());
        }
        bool active = false;
        for (const auto& request : requests_) {
            active |= request.response_.outcome_ == outcome_type::pending;
        }
        active |= !pushes_.empty();
        if (active || !ready || draining_) {
            idle_deadline.reset();
        } else if (!idle_deadline) {
            idle_deadline = deadline_after(now, idle_timeout_);
        }
        if (!active && (draining_ || !ready)) {
            break;
        }
        if (idle_deadline && now >= *idle_deadline) {
            break;
        }
        if (tick.received_ || tick.sent_ || tick.critical_output_progress_ || protocol_progress) {
            if (++consecutive_work_ticks < 8) {
                continue;  // Drain only a bounded amount before yielding to the worker.
            }
            consecutive_work_ticks = 0;
            auto yielded = co_await ruvia::async_asio([this](auto completion) {
                asio::post(io_, [completion = std::move(completion)]() mutable {
                    completion(asio::error_code{});
                });
            });
            if (yielded.error_code()) {
                break;
            }
            continue;
        }
        consecutive_work_ticks = 0;
        auto deadline_value = next_deadline(ready ? std::nullopt : std::optional{connect_deadline});
        if (idle_deadline && (!deadline_value || *idle_deadline < *deadline_value)) {
            deadline_value = idle_deadline;
        }
        const auto wake = co_await session.wait_for_activity(tick, deadline_value);
        if (wake == http3_quic_client_socket_session::wake_reason_type::stopped ||
            wake == http3_quic_client_socket_session::wake_reason_type::fatal ||
            (wake == http3_quic_client_socket_session::wake_reason_type::deadline &&
                !ready && clock_type::now() >= connect_deadline)) {
            break;
        }
        // An application submission can race the timer callback on this
        // worker. Re-evaluate pending requests at the top of the loop before
        // committing idle retirement; a newly admitted request clears it.
    }
    co_return !had_http3 && !stopping_;
}

bool http3_client_connection::sweep(bool requests_may_start, bool early_data_only) {
    bool progress_value = false;
    const auto now = clock_type::now();
    for (auto& request : requests_) {
        if (request.response_.outcome_ != outcome_type::pending) {
            continue;
        }
        if (request.continue_deadline_ && now >= *request.continue_deadline_) {
            request.continue_deadline_.reset();
            if (request.response_state_ != nullptr && request.response_state_->upload_ && !request.response_state_->upload_->output_.stopped_) {
                request.response_state_->upload_->content_released_ = true;
                request.response_state_->upload_->output_.notify_data();
                progress_value = true;
            }
        }
        if (request.cancel_requested_) {
            finish_request(request, outcome_type::cancelled);
            progress_value = true;
        } else if ((request.deadline_ && now >= *request.deadline_) ||
                   (request.write_deadline_ && now >= *request.write_deadline_)) {
            finish_request(request, outcome_type::deadline);
            progress_value = true;
        }
    }
    progress_value |= sweep_pushes();
    progress_value |= receive_peer_streams();
    // Read already-available request streams before applying a newly observed
    // GOAWAY cutoff. A final response head is evidence that the request was
    // processed, even if the body stream has not reached FIN yet.
    progress_value |= receive_requests();
    progress_value |= receive_datagrams();
    auto& engine = attempt().engine_;
    if (const auto goaway = engine.peer_goaway_id()) {
        draining_ = true;
        for (auto& request : requests_) {
            const bool response_started = request.response_state_ != nullptr &&
                                          request.response_state_->http3_response_started_;
            if (request.response_.outcome_ == outcome_type::pending && !response_started &&
                (!request.writer_.stream_id() || *request.writer_.stream_id() >= *goaway)) {
                const auto id = request.writer_.stream_id();
                const bool unprocessed = !id || engine.peer_reports_unprocessed(*id);
                finish_request(request, unprocessed ? outcome_type::request_rejected : outcome_type::protocol_error);
                progress_value = true;
            }
        }
    }
    progress_value |= sweep_pushes();
    progress_value |= flush_push_control();
    if (requests_may_start) {
        progress_value |= drive_request_writers(early_data_only);
        if (!early_data_only) {
            progress_value |= drive_critical_output();
        }
    }
    return progress_value;
}

http_datagram_session_config http3_client_connection::datagram_config(request_id_type id) const {
    require_owner_thread();
    const auto request = find(id);
    if (request == requests_.end() || request->response_state_ == nullptr ||
        !request->response_state_->tunnel_ || !request->response_state_->tunnel_->config_.datagrams_ ||
        !request->writer_.stream_id() || live_session() == nullptr) {
        return {};
    }
    const auto& engine = attempt_->engine_;
    const auto& peer = engine.peer_settings();
    const auto limit = attempt_->session_->transport().max_datagram_payload_size();
    return {
        .http3_stream_id_ = request->writer_.stream_id(),
        .local_h3_datagram_ = engine.local_settings().h3_datagram_,
        .peer_h3_datagram_ = peer && peer->h3_datagram_,
        .quic_datagram_ = limit != 0,
        .max_quic_payload_bytes_ = limit};
}
bool http3_client_connection::send_datagram(request_id_type id, std::span<const std::byte> wire) {
    require_owner_thread();
    const auto request = find(id);
    auto* session = live_session();
    if (request == requests_.end() || request->response_state_ == nullptr || !request->response_state_->tunnel_ ||
        !request->response_state_->tunnel_->accepted_ || request->response_state_->tunnel_->output_.ended_ || request->response_state_->tunnel_->output_.stopped_ || session == nullptr) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    const auto queued = session->transport().write_datagram(wire);
    if (queued == ruvia::quic_datagram_write_status::queued) {
        session->notify_work();
        return true;
    }
    if (queued == ruvia::quic_datagram_write_status::dropped) {
        return false;
    }
    throw std::runtime_error("HTTP Datagram transport is unavailable");
}
bool http3_client_connection::receive_datagrams() {
    bool progress_value{};
    auto& attempt = this->attempt();
    auto& transport = attempt.session_->transport();
    static constexpr auto invalid_datagram = "invalid HTTP/3 Datagram";
    std::array<std::byte, 65536> wire{};
    for (std::size_t count = 0; count < ruvia::quic_limits{}.max_datagrams_; ++count) {
        const auto datagram = transport.read_datagram(wire);
        if (datagram.status_ == ruvia::quic_datagram_status::would_block ||
            datagram.status_ == ruvia::quic_datagram_status::unavailable) {
            break;
        }
        if (datagram.status_ == ruvia::quic_datagram_status::too_large ||
            datagram.size_ > wire.size()) {
            continue;
        }
        progress_value = true;
        const auto bytes_value = std::span<const char>(
            reinterpret_cast<const char*>(wire.data()), datagram.size_);
        const auto decoded = decode_http3_datagram(bytes_value);
        if ((decoded.index() != 0)) {
            fail_connection(outcome_type::protocol_error, http3_datagram_error_code, invalid_datagram);
        }
        const auto request = std::find_if(requests_.begin(), requests_.end(), [&](const auto& current) {
            return current.writer_.stream_id() && *current.writer_.stream_id() == std::get<0>(decoded).stream_id_ && !current.stream_retired_;
        });
        auto* state_value = request == requests_.end() ? nullptr : request->response_state_;
        const auto planned = plan_http3_datagram_receive(std::get<0>(decoded),
            {.local_h3_datagram_ = attempt.engine_.local_settings().h3_datagram_,
                .stream_exists_ = request != requests_.end(),
                .receive_open_ = state_value != nullptr && !state_value->receive_complete() && !state_value->abandoned_,
                .supports_datagrams_ = state_value != nullptr && state_value->tunnel_ && state_value->tunnel_->config_.datagrams_});
        if (planned == http3_datagram_receive_status::connection_error) {
            fail_connection(outcome_type::protocol_error, http3_datagram_error_code, invalid_datagram);
        }
        if (planned == http3_datagram_receive_status::stream_error) {
            (void)transport.reset_stream(std::get<0>(decoded).stream_id_, http3_datagram_error_code);
            finish_request(*request, outcome_type::protocol_error);
        } else if (planned == http3_datagram_receive_status::deliver && state_value->tunnel_->accepted_ &&
                   state_value->tunnel_->datagrams_.size() < ruvia::quic_limits{}.max_datagrams_) {
            state_value->tunnel_->datagrams_.emplace_back(bytes_value.data(), bytes_value.size());
            state_value->data_signal_.notify();
        }
    }
    return progress_value;
}

bool http3_client_connection::replay_rejected_early_streams() {
    auto& session = *attempt().session_;
    std::array<std::uint64_t, 32> rejected{};
    bool progress_value = false;
    while (true) {
        const auto count = session.take_rejected_early_streams(rejected);
        for (std::size_t index = 0; index < count; ++index) {
            const auto stream_id = rejected[index];
            const auto request = std::find_if(requests_.begin(), requests_.end(),
                [stream_id](const request_type& candidate_value) {
                    return candidate_value.writer_.stream_id() == stream_id;
                });
            if (request == requests_.end() || request->response_.outcome_ != outcome_type::pending) {
                continue;
            }
            rearm_early_request(*request);
            progress_value = true;
        }
        if (count < rejected.size()) {
            break;
        }
    }
    return progress_value;
}

bool http3_client_connection::receive_peer_streams() {
    auto& attempt = this->attempt();
    auto& quic = attempt.session_->transport();
    auto& receiver = attempt.receiver_;
    auto& peer_streams = attempt.peer_streams_;
    const auto accepted = quic.accept_streams();
    if (accepted.status_ != ruvia::quic_operation_status::accepted &&
        accepted.status_ != ruvia::quic_operation_status::need_input &&
        accepted.status_ != ruvia::quic_operation_status::would_block) {
        throw std::runtime_error("QUIC client cannot accept peer critical streams");
    }
    for (std::size_t i = 0; i < accepted.size_; ++i) {
        const auto& stream = accepted.streams_[i];
        if (!stream.readable_ || stream.writable_) {
            // RFC 9114 section 6.1: a server-initiated bidirectional stream.
            fail_connection(outcome_type::protocol_error,
                static_cast<std::uint64_t>(http3_connection_error_code::stream_creation_error),
                "invalid peer HTTP/3 stream");
        }
        if (peer_streams.size() == ruvia::quic_limits{}.max_streams_) {
            fail_connection(outcome_type::protocol_error,
                static_cast<std::uint64_t>(http3_connection_error_code::excessive_load),
                "excessive peer HTTP/3 streams");
        }
        peer_streams.push_back(stream.stream_id_);
    }
    bool progress_value = accepted.size_ != 0;
    for (std::size_t index = 0; index < peer_streams.size();) {
        const auto id = peer_streams[index];
        auto push = find_push_by_stream(id);
        std::size_t read_budget = http3_client_receive_driver::read_block_bytes;
        if (push != pushes_.end() && push->delivery_) {
            const auto allowance = push->delivery_->read_allowance(read_budget);
            if (allowance.status_ == http3_client_response_delivery::read_status_type::backpressured ||
                allowance.status_ == http3_client_response_delivery::read_status_type::terminal) {
                ++index;
                continue;
            }
            if (allowance.status_ == http3_client_response_delivery::read_status_type::retirement_required) {
                finish_push(push, outcome_type::protocol_error);
                progress_value = true;
                continue;
            }
            read_budget = allowance.bytes_;
        }
        const auto health = quic.read_health(id);
        const auto part = health.status_ == ruvia::quic_stream_read_status::reset
                              ? receiver.accept_reset(id, health.peer_reset_error_code_)
                              : receiver.drive(id, [&quic](std::uint64_t stream_id, std::span<char> bytes_value) { return quic.read_stream(stream_id, std::as_writable_bytes(bytes_value)); }, read_budget);
        // The prefix may have associated this physical stream during feed.
        push = find_push_by_stream(id);
        if (part.status_ == http3_client_receive_driver::status_type::connection_error) {
            fail_connection(outcome_type::protocol_error, protocol_close_code(part.protocol_), "HTTP/3 peer stream failed");
        }
        if (part.status_ == http3_client_receive_driver::status_type::transport_error) {
            terminal_failure_ = outcome_type::transport_error;
            throw std::runtime_error("HTTP/3 peer stream transport failed");
        }
        if (push != pushes_.end() && (part.status_ == http3_client_receive_driver::status_type::response_complete ||
                                         part.status_ == http3_client_receive_driver::status_type::stream_reset ||
                                         part.status_ == http3_client_receive_driver::status_type::stream_error ||
                                         part.status_ == http3_client_receive_driver::status_type::peer_stream_ended ||
                                         (push->delivery_ && push->delivery_->retirement_reason() != http3_client_response_delivery::retirement_reason_type::none))) {
            finish_push(push, part.status_ == http3_client_receive_driver::status_type::response_complete ? outcome_type::complete : outcome_type::protocol_error);
            progress_value = true;
            continue;
        }
        if (part.status_ == http3_client_receive_driver::status_type::peer_stream_ended ||
            part.status_ == http3_client_receive_driver::status_type::stream_reset ||
            part.status_ == http3_client_receive_driver::status_type::stream_error) {
            const auto closed = quic.close_stream(id);
            if (closed != ruvia::quic_operation_status::accepted &&
                closed != ruvia::quic_operation_status::completed &&
                closed != ruvia::quic_operation_status::retired) {
                throw std::runtime_error("HTTP/3 peer stream could not close");
            }
            receiver.retire(id);
            std::erase_if(attempt.peer_push_streams_, [id](const peer_push_stream_type& binding) { return binding.stream_id_ == id; });
            peer_streams.erase(peer_streams.begin() + static_cast<std::ptrdiff_t>(index));
            progress_value = true;
        } else if (part.status_ == http3_client_receive_driver::status_type::response_complete) {
            throw std::runtime_error("HTTP/3 push response has no promised owner");
        } else {
            progress_value |= part.status_ == http3_client_receive_driver::status_type::progress;
            ++index;
        }
    }
    return progress_value;
}

bool http3_client_connection::receive_requests() {
    bool progress_value = false;
    auto& attempt = this->attempt();
    auto& quic = attempt.session_->transport();
    auto& receiver = attempt.receiver_;
    for (auto& request : requests_) {
        if (request.response_.outcome_ != outcome_type::pending || !request.writer_.stream_id()) {
            continue;
        }
        if (request.response_state_ != nullptr && request.response_state_->tunnel_ && request.response_state_->tunnel_->accepted_ && request.response_state_->tunnel_->receive_ended_) {
            if (request.writer_.finished()) {
                finish_request(request, outcome_type::complete);
                progress_value = true;
            }
            continue;
        }
        const auto health = quic.read_health(*request.writer_.stream_id());
        if (health.status_ == ruvia::quic_stream_read_status::reset) {
            const auto reset = receiver.accept_reset(*request.writer_.stream_id(), health.peer_reset_error_code_);
            if (reset.status_ != http3_client_receive_driver::status_type::stream_reset) {
                throw std::runtime_error("HTTP/3 reset failed to retire blocked response");
            }
            request.response_.peer_reset_error_code_ = reset.peer_reset_error_code_;
            finish_request(request, reset.peer_reports_unprocessed_ ? outcome_type::request_rejected : outcome_type::protocol_error);
            progress_value = true;
            continue;
        }
        if (health.status_ != ruvia::quic_stream_read_status::would_block &&
            health.status_ != ruvia::quic_stream_read_status::fin &&
            health.status_ != ruvia::quic_stream_read_status::data) {
            throw std::runtime_error("HTTP/3 response receive transport failed");
        }
        std::size_t read_budget = http3_client_receive_driver::read_block_bytes;
        if (request.delivery_) {
            const auto allowance = request.delivery_->read_allowance(read_budget);
            if (allowance.status_ == http3_client_response_delivery::read_status_type::backpressured ||
                allowance.status_ == http3_client_response_delivery::read_status_type::terminal) {
                continue;
            }
            if (allowance.status_ == http3_client_response_delivery::read_status_type::retirement_required) {
                const auto reason = request.delivery_->retirement_reason();
                finish_request(request, reason == http3_client_response_delivery::retirement_reason_type::response_too_large
                                            ? outcome_type::response_too_large
                                        : reason == http3_client_response_delivery::retirement_reason_type::callback_failure
                                            ? outcome_type::protocol_error
                                            : outcome_type::protocol_error);
                progress_value = true;
                continue;
            }
            read_budget = allowance.bytes_;
        }
        const auto part = receiver.drive(*request.writer_.stream_id(), [&quic](std::uint64_t id, std::span<char> bytes_value) { return quic.read_stream(id, std::as_writable_bytes(bytes_value)); }, read_budget);
        if (part.status_ == http3_client_receive_driver::status_type::connection_error) {
            fail_connection(part.protocol_.status_ == http3_client_sans_io_session_status::body_limit_exceeded
                                ? outcome_type::response_too_large
                                : outcome_type::protocol_error,
                protocol_close_code(part.protocol_), "HTTP/3 response connection protocol failed");
        }
        if (part.status_ == http3_client_receive_driver::status_type::transport_error) {
            terminal_failure_ = outcome_type::transport_error;
            throw std::runtime_error("HTTP/3 response transport failed");
        }
        if (request.delivery_ &&
            request.delivery_->retirement_reason() !=
                http3_client_response_delivery::retirement_reason_type::none) {
            const auto reason = request.delivery_->retirement_reason();
            finish_request(request, reason == http3_client_response_delivery::retirement_reason_type::response_too_large
                                        ? outcome_type::response_too_large
                                    : reason == http3_client_response_delivery::retirement_reason_type::callback_failure
                                        ? outcome_type::protocol_error
                                        : outcome_type::protocol_error);
            progress_value = true;
            continue;
        }
        switch (part.status_) {
            case http3_client_receive_driver::status_type::progress:
                progress_value = true;
                break;
            case http3_client_receive_driver::status_type::blocked:
                break;
            case http3_client_receive_driver::status_type::response_complete:
                if (request.response_state_ != nullptr && request.response_state_->tunnel_ && request.response_state_->tunnel_->accepted_) {
                    request.response_state_->tunnel_->receive_ended_ = true;
                    request.response_state_->data_signal_.notify();
                }
                if (request.response_state_ == nullptr || !request.response_state_->tunnel_ || !request.response_state_->tunnel_->accepted_ || request.writer_.finished()) {
                    finish_request(request, outcome_type::complete);
                }
                progress_value = true;
                break;
            case http3_client_receive_driver::status_type::stream_reset:
                request.response_.peer_reset_error_code_ = part.peer_reset_error_code_;
                finish_request(request, part.peer_reports_unprocessed_ &&
                                                (request.response_state_ == nullptr ||
                                                    !request.response_state_->http3_response_started_)
                                            ? outcome_type::request_rejected
                                            : outcome_type::protocol_error);
                progress_value = true;
                break;
            case http3_client_receive_driver::status_type::stream_error:
                finish_request(request, outcome_type::protocol_error);
                progress_value = true;
                break;
            case http3_client_receive_driver::status_type::connection_error:
                fail_connection(part.protocol_.status_ == http3_client_sans_io_session_status::body_limit_exceeded
                                    ? outcome_type::response_too_large
                                    : outcome_type::protocol_error,
                    protocol_close_code(part.protocol_), "HTTP/3 response connection protocol failed");
            default:
                throw std::runtime_error("HTTP/3 response stream or QUIC connection failed");
        }
    }
    reap_released_response_requests();
    return progress_value;
}

bool http3_client_connection::drive_critical_output() {
    bool progress_value = false;
    auto& attempt = this->attempt();
    auto& engine = attempt.engine_;
    for (std::size_t i = 0; i < attempt.critical_output_.size(); ++i) {
        auto& write_deadline = attempt.critical_write_deadlines_[i];
        if (write_deadline && clock_type::now() >= *write_deadline) {
            // A local inactivity bound, not a peer error to report.
            fail_connection(outcome_type::deadline,
                static_cast<std::uint64_t>(http3_connection_error_code::no_error),
                "HTTP/3 critical stream write timeout");
        }
        auto& output = attempt.critical_output_[i];
        auto& offset = attempt.critical_output_offset_[i];
        if (output.empty()) {
            const auto pending = i == 0 ? engine.pending_encoder_output() : i == 1 ? engine.pending_decoder_output()
                                                                                   : engine.pending_control_output();
            if (pending.empty()) {
                continue;
            }
            output.assign(pending.data(), std::min(pending.size(), std::size_t{16 * 1024}));
            offset = 0;
            if (write_timeout_ && !write_deadline) {
                write_deadline = deadline_after(clock_type::now(), *write_timeout_);
            }
        }
        const auto remaining = std::span<const char>(output.data(), output.size()).subspan(offset);
        const auto kind = i == 0 ? ruvia::http3_critical_stream_output::stream_kind::qpack_encoder : i == 1 ? ruvia::http3_critical_stream_output::stream_kind::qpack_decoder
                                                                                                            : ruvia::http3_critical_stream_output::stream_kind::control;
        const auto sent = attempt.session_->write_critical_stream(kind, remaining);
        if (sent.status_ == ruvia::quic_operation_status::would_block ||
            sent.status_ == ruvia::quic_operation_status::need_input) {
            continue;
        }
        if (sent.status_ != ruvia::quic_operation_status::accepted ||
            sent.accepted_ > remaining.size()) {
            throw std::runtime_error("HTTP/3 critical stream write failed");
        }
        const bool consumed = i == 0 ? engine.consume_encoder_output(sent.accepted_) : i == 1 ? engine.consume_decoder_output(sent.accepted_)
                                                                                              : engine.consume_control_output(sent.accepted_);
        if (!consumed) {
            throw std::logic_error("HTTP/3 critical stream acknowledgement mismatch");
        }
        offset += sent.accepted_;
        progress_value |= sent.accepted_ != 0;
        if (sent.accepted_ != 0 && write_timeout_) {
            write_deadline = deadline_after(clock_type::now(), *write_timeout_);
        }
        if (offset == output.size()) {
            std::pmr::string(output.get_allocator()).swap(output);
            offset = 0;
            write_deadline.reset();
        }
    }
    return progress_value;
}

bool http3_client_connection::drive_request_writers(bool early_data_only) {
    bool progress_value = false;
    auto& attempt = this->attempt();
    auto& engine = attempt.engine_;
    auto& quic = attempt.session_->transport();
    for (auto& request : requests_) {
        if (request.response_.outcome_ != outcome_type::pending ||
            (early_data_only && !request.early_data_eligible_)) {
            continue;
        }
        // Response priority remains meaningful after the request FIN. In
        // particular, an accepted 0-RTT writer can already be finished when its
        // deferred control output becomes eligible at handshake completion.
        if (!early_data_only && request.pending_priority_update_ &&
            request.response_parser_registered_ && request.writer_.stream_id()) {
            if (!engine.queue_priority_update(
                    *request.writer_.stream_id(), *request.pending_priority_update_)) {
                throw std::runtime_error("HTTP/3 request priority update could not be queued");
            }
            request.pending_priority_update_.reset();
            progress_value = true;
        }
        if (request.writer_.finished()) {
            continue;
        }
        if (draining_ && !request.writer_.stream_id()) {
            // Local admission stopped before a stream or any request bytes
            // existed. Preserve the same handoff path as explicit peer rejection.
            finish_request(request, outcome_type::request_rejected);
            progress_value = true;
            continue;
        }
        if (request.writer_.requires_connect_settings()) {
            if (!engine.peer_settings()) {
                continue;
            }
            if (!engine.peer_settings()->enable_connect_protocol_) {
                finish_request(request, outcome_type::invalid_request);
                progress_value = true;
                continue;
            }
        }
        auto* upload = request.response_state_ != nullptr ? request.response_state_->output() : nullptr;
        if (const auto id = request.writer_.stream_id();
            id && quic.write_health(*id) == ruvia::quic_operation_status::stream_closed) {
            // RFC 9114 section 4.1: the peer may stop reading the request
            // (STOP_SENDING) and still send a complete response, which the
            // client must not discard. Only this send half is closed.
            if (upload != nullptr) {
                upload->stop();
            }
            request.writer_.stop_sending();
            request.write_deadline_.reset();
            request.continue_deadline_.reset();
            progress_value = true;
            continue;
        }
        if (upload != nullptr && upload->stopped_ && !upload->ended_) {
            if (const auto id = request.writer_.stream_id()) {
                const auto reset = quic.reset_stream(
                    *id, static_cast<std::uint64_t>(http3_connection_error_code::no_error));
                if (reset == ruvia::quic_operation_status::would_block ||
                    reset == ruvia::quic_operation_status::need_input) {
                    continue;
                }
                if (reset != ruvia::quic_operation_status::accepted) {
                    throw std::runtime_error("HTTP/3 upload send reset failed");
                }
            }
            request.writer_.stop_sending();
            request.write_deadline_.reset();
            request.continue_deadline_.reset();
            progress_value = true;
            continue;
        }
        if (write_timeout_ && !request.write_deadline_ && !request.writer_.waiting_for_content()) {
            request.write_deadline_ = deadline_after(clock_type::now(), *write_timeout_);
        }
        const auto write = request.writer_.drive(
            [&quic] { return quic.open_stream(false); },
            [&engine, &request](std::uint64_t id, http_known_method method) {
                const auto sink_value = request.delivery_
                                            ? http3_client_response_event_sink{
                                                  .callback_ = on_response_event, .context_ = &request}
                                            : http3_client_response_event_sink{};
                const auto registered = engine.register_request(id, method, sink_value);
                if (registered.scope_ != http3_connection_error_scope::none) {
                    return false;
                }
                request.response_parser_registered_ = true;
                return request.writer_.prepare_connection_head(id, engine);
            },
            [&quic](std::uint64_t id, std::span<const char> bytes_value) {
                return quic.write_stream(id, std::as_bytes(bytes_value));
            },
            [&quic](std::uint64_t id) { return quic.finish_stream(id); });
        if (write == http3_client_request_driver::result_type::progress ||
            write == http3_client_request_driver::result_type::finished) {
            request.write_deadline_ = write == http3_client_request_driver::result_type::finished ||
                                              !write_timeout_
                                          ? std::nullopt
                                          : std::optional{deadline_after(clock_type::now(), *write_timeout_)};
            progress_value = true;
        } else if (write == http3_client_request_driver::result_type::connection_draining) {
            draining_ = true;
            finish_request(request, outcome_type::request_rejected);
            progress_value = true;
        } else if (write == http3_client_request_driver::result_type::fatal) {
            throw std::runtime_error("HTTP/3 request write or registration failed");
        }
        if (request.writer_.waiting_for_content()) {
            request.write_deadline_.reset();
            if (request.response_state_ != nullptr && request.response_state_->upload_ && !request.response_state_->upload_->content_released_ && !request.continue_deadline_) {
                request.continue_deadline_ = deadline_after(clock_type::now(), std::chrono::duration_cast<clock_type::duration>(request.response_state_->upload_->config_.continue_timeout_));
            }
        }
    }
    return progress_value;
}

}  // namespace ruvia::detail
