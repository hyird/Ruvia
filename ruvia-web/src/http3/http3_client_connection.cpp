#include "http3/http3_client_connection.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/web/http_client_types.h"

#include "client/http_client_pool.h"
#include "client/http_client_response_decoding.h"
#include "client/http_client_upload_state.h"

namespace ruvia::detail {
namespace {
using clock_type = std::chrono::steady_clock;
constexpr std::size_t max_buffered_connection_body_bytes = 64 * 1024 * 1024;
using session_outcome_type = http3_client_session_driver::outcome_type;

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

http3_client_connection::outcome_type connection_outcome(session_outcome_type outcome) noexcept {
    using outcome_type = http3_client_connection::outcome_type;
    switch (outcome) {
        case session_outcome_type::cancelled:
            return outcome_type::cancelled;
        case session_outcome_type::deadline:
            return outcome_type::deadline;
        case session_outcome_type::resolve_failed:
        case session_outcome_type::connect_failed:
            return outcome_type::connect_failed;
        case session_outcome_type::protocol_error:
            return outcome_type::protocol_error;
        case session_outcome_type::response_too_large:
            return outcome_type::response_too_large;
        case session_outcome_type::transport_error:
            return outcome_type::transport_error;
    }
    return outcome_type::transport_error;
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
    task_scope& pool_tasks, http3_quic_client_tls_context& tls, std::string_view host,
    std::string_view tls_host, http_origin_view origin, std::string_view authority,
    std::chrono::milliseconds connect_timeout, std::pmr::memory_resource* resource,
    std::size_t max_requests, std::size_t max_response_bytes,
    std::chrono::milliseconds idle_timeout, http3_body_budget* receive_body_budget,
    std::optional<std::chrono::milliseconds> write_timeout,
    lifecycle_notification_type lifecycle_notification, http3_qpack_config qpack,
    http3_client_origin_observer origin_observer, http3_client_push_observer push_observer,
    ruvia::quic_version initial_version, bool enable_early_data)
    : owner_thread_(std::this_thread::get_id()),
      worker_(worker_value),
      pool_tasks_(pool_tasks),
      resource_(require_resource(resource)),
      authority_(authority),
      write_timeout_(checked_timeout(write_timeout)),
      max_requests_(max_requests),
      max_response_bytes_(max_response_bytes),
      body_budget_(max_buffered_connection_body_bytes),
      receive_body_budget_(receive_body_budget == nullptr ? &body_budget_ : receive_body_budget),
      lifecycle_notification_(lifecycle_notification),
      origin_observer_(origin_observer),
      push_observer_(push_observer),
      receive_body_budget_wake_(*receive_body_budget_, on_receive_body_budget_released, this),
      requests_(resource_),
      pushes_(resource_),
      peer_push_streams_(resource_),
      driver_(io, tls,
          {.host_ = host,
              .port_ = origin.port(),
              .tls_host_ = tls_host,
              .connect_timeout_ = checked_timeout(connect_timeout),
              .idle_timeout_ = checked_timeout(idle_timeout),
              .write_timeout_ = write_timeout_,
              .limits_ = response_limits(max_requests, max_response_bytes, qpack, origin_observer.receive_ != nullptr, true),
              .initial_version_ = initial_version,
              .enable_early_data_ = enable_early_data},
          resource_, &body_budget_) {
    if (!worker_.valid() || origin.scheme() != http_scheme::https || authority_.empty() ||
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
    if (push_observer_.receive_ != nullptr) {
        if (push_observer_.config_.timeout_) {
            (void)checked_timeout(*push_observer_.config_.timeout_);
        }
        peer_push_streams_.reserve(ruvia::quic_limits{}.max_streams_);
    }
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
    driver_.wake();
}

void http3_client_connection::on_origin_event(void* context_value, const http3_connection_event& event) {
    auto& owner_value = *static_cast<http3_client_connection*>(context_value);
    if (event.origin_advertisement_ != nullptr) {
        owner_value.origin_observer_.receive_(owner_value.origin_observer_.context_,
            owner_value.origin_observer_.connection_slot_, *event.origin_advertisement_);
    }
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
    if (driver_.stopping() || draining_ || terminal_ ||
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
            response->head_ready_ || response->failure_ ||
            response->transport_ != http_client_response_transport::unassigned ||
            response->has_http3_body_budget() || !response->buffered_.empty() ||
            !response->pending_.empty())) {
        return {.outcome_ = outcome_type::invalid_request};
    }
    const auto method = classify_http_method(request.method());
    const bool replay_safe = request.replay_safe();
    const bool early_data_eligible = replay_safe && !request.is_tunnel() &&
                                     request.upload() == nullptr && !request.has_body() &&
                                     (method == http_known_method::get || method == http_known_method::head);
    auto owned = http3_client_request_write::create(std::move(request), authority_, resource_);
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
    driver_.wake();  // Without a session, re-arms DNS to include a newly earlier deadline.
    return {.id_ = id};
}

void http3_client_connection::start() {
    require_owner_thread();
    const bool has_pending = std::any_of(requests_.begin(), requests_.end(),
        [](const request_type& request) {
            return request.response_.outcome_ == outcome_type::pending;
        });
    if (starting_ || running_ || driver_.stopping() || terminal_ || !has_pending) {
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
        if (driver_.stopping() || terminal_ || push->cancel_requested_ || !driver_.has_attempt() ||
            !driver_.engine().queue_push_priority_update(push->push_id_, priority)) {
            return false;
        }
        wake_receive_driver();
        return true;
    }
    const auto request = find(id);
    if (driver_.stopping() || terminal_ || request == requests_.end() ||
        request->response_.outcome_ != outcome_type::pending || !request->writer_.stream_id()) {
        return false;
    }
    // A 0-RTT stream defers the update until its attempt's handshake completes.
    if (auto* session = driver_.session(); !driver_.has_attempt() || (session != nullptr &&
                                                                         session->transport().info().state_ !=
                                                                             ruvia::quic_connection_state::ready)) {
        request->pending_priority_update_ = priority;
        wake_receive_driver();
        return true;
    }
    if (!driver_.engine().queue_priority_update(*request->writer_.stream_id(), priority)) {
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
        driver_.wake();
        return;
    }
    found->cancel_requested_ = true;
    if (auto* session = driver_.session()) {
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
            state_value->failure_ ||
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
        if (auto* session = driver_.session()) {
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
        if (auto* session = driver_.session()) {
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
        if (auto* session = driver_.session()) {
            session->notify_work();
        }
    }
    maybe_release_response_request(request);
}

ruvia::quic_path_migration http3_client_connection::start_path_migration(const ruvia::quic_address& local_address) {
    require_owner_thread();
    auto* session = driver_.session();
    if (driver_.stopping() || session == nullptr || !running_) {
        return {.status_ = ruvia::quic_migration_status::rejected};
    }
    auto migration = session->start_path_migration(local_address);
    if (migration.status_ == ruvia::quic_migration_status::started ||
        migration.status_ == ruvia::quic_migration_status::validated) {
        quic_connection_migration_id_ = migration.id_;
        quic_path_migration_generation_ = driver_.generation();
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
    if (const auto* session = driver_.session(); session != nullptr && quic_path_migration_generation_ == driver_.generation()) {
        if (const auto migration = session->path_migration(quic_connection_migration_id_)) {
            auto result_value = *migration;
            result_value.id_ = id;
            return result_value;
        }
    }
    return quic_path_migration_;
}

std::optional<ruvia::quic_path_migration> http3_client_connection::active_path_migration() const noexcept {
    if (const auto* session = driver_.session(); quic_path_migration_ && session != nullptr &&
                                                 quic_path_migration_generation_ == driver_.generation()) {
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
    auto* session = driver_.session();
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
    const auto* session = driver_.session();
    if (session == nullptr || !quic_path_migration_ ||
        quic_path_migration_generation_ != driver_.generation()) {
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
    driver_.request_stop();
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
    for (const auto& stream : peer_push_streams_) {
        if (stream.deadline_ && std::none_of(pushes_.begin(), pushes_.end(), [&stream](const push_type& push) { return push.stream_id_ == stream.stream_id_; }) &&
            (!connect_deadline || *stream.deadline_ < *connect_deadline)) {
            connect_deadline = stream.deadline_;
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
        request.writer_.stop_sending();
        request.stream_retired_ = true;
        return true;
    }
    if (!driver_.has_attempt()) {
        // The driver already retired the whole attempt: no peer input can
        // reach this stream or the parser state that the attempt owned.
        request.writer_.stop_sending();
        request.response_parser_registered_ = false;
        request.stream_retired_ = true;
        return true;
    }
    auto& engine = driver_.engine();
    try {
        if (auto* session = driver_.session()) {
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
        request.writer_.stop_sending();
        if (!request.response_parser_registered_) {
            // The QUIC stream has now been physically retired, but registration
            // never installed parser state (for example, allocation failed).
            request.stream_retired_ = true;
            return true;
        }
        auto parsed_value = engine.response(*id);
        if (!parsed_value) {
            (void)engine.cancel_request(*id);
            parsed_value = engine.response(*id);
        }
        if (!parsed_value) {
            if (driver_.session() != nullptr) {
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
        driver_.receiver().retire(*id);
        request.stream_retired_ = engine.release(*id);
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
        auto& engine = driver_.engine();
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

    if (!retire_request(request, outcome == outcome_type::complete) && driver_.session() != nullptr) {
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
                                  driver_.terminal_failure() != session_outcome_type::protocol_error && state_value != nullptr &&
                                  state_value->output() == nullptr && !state_value->http3_response_started_ && !state_value->head_ready_ &&
                                  !state_value->complete_ && !state_value->failure_ &&
                                  !state_value->failure_ && state_value->producer_body_bytes() == 0 &&
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
        } else if (driver_.terminal_failure() == session_outcome_type::protocol_error) {
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
                        plan && plan->content_semantics() == http_response_content_semantics::with_content;
                    // Only a decode-required body is withheld from incremental
                    // readers; a streamed body may be borrowed across a pipe write.
                    const bool body_withheld = state_value->body_decode_required_;
                    decode_http_client_response_body(
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
    if (delivery != nullptr && !handoff_rejected && state_value->pool_ != nullptr) {
        // Record the pool request's result before any consumer can observe
        // this completion; its waiter resumes only after posted wakeups.
        state_value->pool_->finish_http3_request(*state_value, outcome == outcome_type::complete);
    }
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
    const auto failure = connection_outcome(co_await driver_.run(*this));
    // Admission ends before the closing period can suspend this driver, so a
    // late submission waits for the next connection instead of this outcome.
    draining_ = true;
    if (driver_.has_attempt()) {
        cache_path_migration();
        co_await driver_.close();
    }
    while (!pushes_.empty()) {
        finish_push(pushes_.begin(), failure);
    }
    finish_all(failure);
    driver_.reset_attempt();
    terminal_ = true;
    running_ = false;
    if (lifecycle_notification_.notify_ != nullptr) {
        lifecycle_notification_.notify_(lifecycle_notification_.context_);
    }
}

void http3_client_connection::expire(time_point_type now) {
    for (auto& request : requests_) {
        if (request.response_.outcome_ == outcome_type::pending && request.deadline_ && now >= *request.deadline_) {
            finish_request(request, outcome_type::deadline);
        }
    }
}

bool http3_client_connection::active() const noexcept {
    return !pushes_.empty() || std::any_of(requests_.begin(), requests_.end(), [](const request_type& request) {
        return request.response_.outcome_ == outcome_type::pending;
    });
}

void http3_client_connection::attempt_started(http3_client_sans_io_session_engine& engine) {
    peer_push_streams_.clear();
    seen_pushes_.reset();
    settled_pushes_.reset();
    cancelled_pushes_.reset();
    authorized_push_id_ = 0;
    pending_push_credits_ = 0;
    if (push_observer_.receive_ != nullptr) {
        authorized_push_id_ = push_observer_.config_.max_concurrent_pushes_ - 1;
        if (!engine.queue_max_push_id(authorized_push_id_)) {
            throw std::logic_error("HTTP/3 initial push authorization failed");
        }
        engine.observe_pushes(on_push_event, this);
    }
    if (origin_observer_.receive_ != nullptr) {
        engine.observe_origins(on_origin_event, this);
    }
    // GOAWAY and closing state belonged to the previous QUIC connection.
    draining_ = false;
}

void http3_client_connection::attempt_abandoned() {
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
    auto& engine = driver_.engine();
    const auto stream_id = *request.writer_.stream_id();
    const bool response_started = request.response_state_ != nullptr &&
                                  request.response_state_->http3_response_started_;
    if (!request.early_data_eligible_ || response_started ||
        !request.response_parser_registered_ || !engine.cancel_request(stream_id)) {
        throw std::runtime_error("HTTP/3 cannot replay an ineligible or committed 0-RTT request");
    }
    driver_.receiver().retire(stream_id);
    if (!engine.release(stream_id)) {
        throw std::logic_error("HTTP/3 replayed 0-RTT response state could not be retired");
    }
    request.response_parser_registered_ = false;
    request.stream_retired_ = true;
    request.write_deadline_.reset();
    if (request.cancel_requested_) {
        finish_request(request, outcome_type::cancelled);
        return;
    }
    if (!request.writer_.replay_on_new_stream(authority_, resource_)) {
        finish_request(request, outcome_type::transport_error);
        return;
    }
    request.stream_retired_ = false;
}

bool http3_client_connection::early_stream_rejected(std::uint64_t stream_id) {
    const auto request = std::find_if(requests_.begin(), requests_.end(),
        [stream_id](const request_type& candidate_value) {
            return candidate_value.writer_.stream_id() == stream_id;
        });
    if (request == requests_.end() || request->response_.outcome_ != outcome_type::pending) {
        return false;
    }
    rearm_early_request(*request);
    return true;
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
    progress_value |= driver_.receive_peer_streams(*this);
    // Read already-available request streams before applying a newly observed
    // GOAWAY cutoff. A final response head is evidence that the request was
    // processed, even if the body stream has not reached FIN yet.
    progress_value |= receive_requests();
    progress_value |= receive_datagrams();
    auto& engine = driver_.engine();
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
    }
    return progress_value;
}

http_datagram_session_config http3_client_connection::datagram_config(request_id_type id) const {
    require_owner_thread();
    const auto request = find(id);
    if (request == requests_.end() || request->response_state_ == nullptr ||
        !request->response_state_->tunnel_ || !request->response_state_->tunnel_->extended_connect_ ||
        !request->writer_.stream_id() || driver_.session() == nullptr) {
        return {};
    }
    const auto& engine = driver_.engine();
    const auto& peer = engine.peer_settings();
    const auto limit = driver_.session()->transport().max_datagram_payload_size();
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
    auto* session = driver_.session();
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
    auto& engine = driver_.engine();
    auto& transport = driver_.session()->transport();
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
            driver_.fail(session_outcome_type::protocol_error, http3_datagram_error_code, invalid_datagram);
        }
        const auto request = std::find_if(requests_.begin(), requests_.end(), [&](const auto& current) {
            return current.writer_.stream_id() && *current.writer_.stream_id() == std::get<0>(decoded).stream_id_ && !current.stream_retired_;
        });
        auto* state_value = request == requests_.end() ? nullptr : request->response_state_;
        const auto planned = plan_http3_datagram_receive(std::get<0>(decoded),
            {.local_h3_datagram_ = engine.local_settings().h3_datagram_,
                .stream_exists_ = request != requests_.end(),
                .receive_open_ = state_value != nullptr && !state_value->receive_complete() && !state_value->abandoned_,
                .supports_datagrams_ = state_value != nullptr && state_value->tunnel_ && state_value->tunnel_->extended_connect_});
        if (planned == http3_datagram_receive_status::connection_error) {
            driver_.fail(session_outcome_type::protocol_error, http3_datagram_error_code, invalid_datagram);
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

std::size_t http3_client_connection::peer_stream_read_budget(std::uint64_t stream_id) {
    const auto push = find_push_by_stream(stream_id);
    if (push == pushes_.end() || !push->delivery_) {
        return http3_client_receive_driver::read_block_bytes;
    }
    const auto allowance = push->delivery_->read_allowance(http3_client_receive_driver::read_block_bytes);
    switch (allowance.status_) {
        case http3_client_response_delivery::read_status_type::backpressured:
        case http3_client_response_delivery::read_status_type::terminal:
            return 0;
        case http3_client_response_delivery::read_status_type::retirement_required:
            finish_push(push, outcome_type::protocol_error);
            return 0;
        default:
            return allowance.bytes_;
    }
}

bool http3_client_connection::peer_stream_input(std::uint64_t stream_id, const http3_client_receive_driver::result_type& part) {
    using status_type = http3_client_receive_driver::status_type;
    const bool ended = part.status_ == status_type::response_complete || part.status_ == status_type::stream_reset ||
                       part.status_ == status_type::stream_error || part.status_ == status_type::peer_stream_ended;
    // The prefix may have associated this physical stream during feed.
    const auto push = find_push_by_stream(stream_id);
    if (push != pushes_.end() &&
        (ended || (push->delivery_ && push->delivery_->retirement_reason() != http3_client_response_delivery::retirement_reason_type::none))) {
        finish_push(push, part.status_ == status_type::response_complete ? outcome_type::complete : outcome_type::protocol_error);
        return true;
    }
    if (ended) {
        std::erase_if(peer_push_streams_, [stream_id](const peer_push_stream_type& binding) { return binding.stream_id_ == stream_id; });
    }
    return false;
}

bool http3_client_connection::receive_requests() {
    bool progress_value = false;
    auto& quic = driver_.session()->transport();
    auto& receiver = driver_.receiver();
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
            driver_.fail(part.protocol_.status_ == http3_client_sans_io_session_status::body_limit_exceeded
                             ? session_outcome_type::response_too_large
                             : session_outcome_type::protocol_error,
                http3_client_protocol_close_code(part.protocol_), "HTTP/3 response connection protocol failed");
        }
        if (part.status_ == http3_client_receive_driver::status_type::transport_error) {
            driver_.fail(session_outcome_type::transport_error, std::nullopt, "HTTP/3 response transport failed");
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
                driver_.fail(part.protocol_.status_ == http3_client_sans_io_session_status::body_limit_exceeded
                                 ? session_outcome_type::response_too_large
                                 : session_outcome_type::protocol_error,
                    http3_client_protocol_close_code(part.protocol_), "HTTP/3 response connection protocol failed");
            default:
                throw std::runtime_error("HTTP/3 response stream or QUIC connection failed");
        }
    }
    reap_released_response_requests();
    return progress_value;
}

bool http3_client_connection::drive_request_writers(bool early_data_only) {
    bool progress_value = false;
    auto& engine = driver_.engine();
    auto& quic = driver_.session()->transport();
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
