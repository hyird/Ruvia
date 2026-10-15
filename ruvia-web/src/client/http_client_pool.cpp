#include "client/http_client_pool.h"

#include <exception>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request_target.h"

#include "client/client_tls.h"
#include "client/http1_client_exchange.h"
#include "client/http_client_connection.h"
#include "client/http_client_response_state.h"
#include "client/http_client_result_budget.h"
#include "http3/http3_client_connection.h"

namespace ruvia::detail {
namespace {
void throw_if_abandoned(const http_client_response_state& state_value) {
    if (state_value.abandoned_) {
        throw http_client_error(
            http_client_error::code_type::cancelled, "HTTP request abandoned before transport admission");
    }
}

[[nodiscard]] std::size_t http_client_scheduler_slots(const http_client_config_storage& config) noexcept {
    if (config.protocol_ == http_client_protocol::http1_only) {
        return config.connection_count_;
    }
    if (config.protocol_ == http_client_protocol::http3_only) {
        return config.connection_count_ * client_quic_connections::requests_per_connection;
    }
    return config.connection_count_ * config.max_concurrent_http2_streams_per_connection_;
}

[[nodiscard]] http_client_error::code_type http3_client_error_code(http3_client_connection::outcome_type outcome) noexcept {
    using outcome_type = http3_client_connection::outcome_type;
    switch (outcome) {
        case outcome_type::cancelled:
            return http_client_error::code_type::cancelled;
        case outcome_type::deadline:
            return http_client_error::code_type::timeout;
        case outcome_type::connect_failed:
            return http_client_error::code_type::connect_failed;
        case outcome_type::transport_error:
            return http_client_error::code_type::io_error;
        case outcome_type::response_too_large:
            return http_client_error::code_type::response_too_large;
        case outcome_type::result_budget_exceeded:
            return http_client_error::code_type::result_budget_exceeded;
        case outcome_type::queue_full:
            return http_client_error::code_type::queue_full;
        case outcome_type::connection_draining:
        case outcome_type::request_rejected:
        case outcome_type::invalid_request:
        case outcome_type::protocol_error:
            return http_client_error::code_type::protocol_error;
        case outcome_type::pending:
        case outcome_type::complete:
            break;
    }
    return http_client_error::code_type::protocol_error;
}

[[nodiscard]] std::shared_ptr<http_client_result_budget_domain> require_http_client_result_budget_domain(
    std::shared_ptr<http_client_result_budget_domain> result_budget_domain) {
    if (!result_budget_domain) {
        throw std::invalid_argument("HTTP client result budget domain must not be null");
    }
    return result_budget_domain;
}

[[nodiscard]] task<std::size_t> acquire_http1_exchange_slot(
    pool_lease_scheduler& scheduler, const operation_timeout& timeout, stop_token stop_token_value) {
    const auto result_value = co_await scheduler.acquire(timeout.remaining(), std::move(stop_token_value));
    if (result_value.has_value()) {
        co_return result_value.value();
    }
    if (result_value.status() == worker_wait_status::timed_out) {
        throw http_client_error(http_client_error::code_type::timeout, "HTTP/1 connection acquire timed out");
    }
    if (result_value.status() == worker_wait_status::cancelled) {
        throw http_client_error(http_client_error::code_type::cancelled, "HTTP/1 request cancelled");
    }
    if (result_value.status() == worker_wait_status::closed) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP client pool is closing");
    }
    std::terminate();
}

}  // namespace

void http_client_slot_policy::throw_acquire_failure(const http_client_pool&, worker_wait_status status) {
    switch (status) {
        case worker_wait_status::timed_out:
            throw http_client_error(
                http_client_error::code_type::timeout, "http client connection pool acquire timed out");
        case worker_wait_status::cancelled:
            throw http_client_error(http_client_error::code_type::cancelled, "http client request cancelled");
        case worker_wait_status::closed:
            throw http_client_error(http_client_error::code_type::closing, "http client pool is closing");
        case worker_wait_status::value:
        case worker_wait_status::worker_stopping:
            break;
    }
    std::terminate();
}

void http_client_slot_policy::throw_aborted(const http_client_pool&, pool_slot_abort_reason reason) {
    if (reason == pool_slot_abort_reason::cancelled) {
        throw http_client_error(http_client_error::code_type::cancelled, "http client request cancelled");
    }
    throw http_client_error(http_client_error::code_type::closing, "http client pool is closing");
}

http_client_pool::http_client_pool(asio::io_context& io_context, const worker_handle& worker_value,
    http_client_config_storage config, std::shared_ptr<http_client_result_budget_domain> result_budget_domain,
    std::pmr::memory_resource* resource, http_client_response_storage response_storage)
    : io_context_(io_context),
      worker_(worker_value),
      resource_(pmr_resource_or_default(resource)),
      config_(std::move(config)),
      origin_(config_.origin()),
      policy_(config_, resource_),
      advertisements_(worker_, config_.advertisements_, resource_),
      pushes_(resource_),
      result_budget_domain_(require_http_client_result_budget_domain(std::move(result_budget_domain))),
      tls_context_(asio::ssl::context::tls_client),
      slots_(*this, http_client_scheduler_slots(config_), worker_, resource_),
      cancellation_target_(make_worker_cancellation_target(*this, worker_)),
      background_tasks_(worker_, {.resource_ = resource_}),
      connections_(resource_),
      quic_(io_context_, worker_, background_tasks_, config_, advertisements_,
          config_.push_.enabled_ ? http3_client_push_observer{
                                       .context_ = this,
                                       .config_ = config_.push_,
                                       .receive_ = [](void* raw, std::size_t slot, http3_client_connection& connection, std::uint64_t id, const http3_message_head& head) { return static_cast<http_client_pool*>(raw)->accept_http3_push(slot, connection, id, head); },
                                       .finished_ = [](void* raw) noexcept { --static_cast<http_client_pool*>(raw)->active_pushes_; },
                                   }
                                 : http3_client_push_observer{},
          resource_) {
    if (config_.protocol_ != http_client_protocol::http3_only) {
        if (config_.scheme_ == http_scheme::https) {
            configure_client_tls_context(*tls_context_.native_handle(), config_.tls_);
        }
        connections_.reserve(config_.connection_count_);
        for (std::size_t index = 0; index < config_.connection_count_; ++index) {
            connections_.push_back(make_pmr_object<http_client_connection>(resource_, *this, index));
        }
    }
    response_memory_ = response_storage == http_client_response_storage::worker_resource
                           ? http_client_response_memory_domain::create(worker_, result_budget_domain_, *resource_)
                           : http_client_response_memory_domain::create_response_owned(worker_, result_budget_domain_);
}

http_client_pool::~http_client_pool() {
    close_now();
}

void http_client_pool::close_now() noexcept {
    if (!slots_.close()) {
        return;
    }
    // slots_.close() retires queued waiters and connection shutdown aborts
    // every connect and exchange; a transferred slot fails at admission.
    quic_.request_stop();
    background_tasks_.request_stop();
    for (auto& connection_value : connections_) {
        connection_value->shutdown();
    }
}

task<void> http_client_pool::join() {
    if (background_joined_) {
        co_return;
    }
    background_joined_ = true;
    std::exception_ptr failure;
    try {
        co_await background_tasks_.join();
    } catch (...) {
        // A child failure is observable only after join retired every child.
        // If join itself could not start, its live drivers still borrow the
        // transport owners below: this is a terminal ownership violation.
        if (background_tasks_.size() != 0) {
            std::terminate();
        }
        failure = std::current_exception();
    }
    // Consumers own their response storage independently of transport. Retire
    // every client borrow, including completed responses, before its owners.
    response_memory_->detach_transport_bindings(*this);
    pushes_.clear();
    advertisements_.retire();
    response_memory_.reset();
    // Destroy QUIC SSL/socket/session owners while the worker loop and its PMR
    // owner are still alive. http_client_pool itself is later destroyed by the
    // application lifecycle thread after the worker has joined.
    quic_.retire();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

ruvia::quic_path_migration http_client_pool::start_quic_path_migration(const ruvia::quic_address& local_address) {
    if (background_joined_) {
        return {.status_ = ruvia::quic_migration_status::rejected};
    }
    return quic_.start_path_migration(local_address);
}

std::optional<ruvia::quic_path_migration> http_client_pool::path_migration(std::uint64_t id) const noexcept {
    return quic_.path_migration(id);
}

ruvia::quic_operation_status http_client_pool::cancel_quic_path_migration(std::uint64_t id) {
    return quic_.cancel_path_migration(id);
}

http_client_stats http_client_pool::stats() const noexcept {
    return {requests_buffered_, requests_in_flight_, completed_requests_, failed_requests_, wire_counters_.sent_,
        wire_counters_.received_, advertisements_.dropped(), received_pushes_, rejected_pushes_};
}

void http_client_pool::cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id == 0 || quic_.cancel(cancellation_id)) {
        return;
    }
    for (auto& connection_value : connections_) {
        if (connection_value->cancel_by_id(cancellation_id)) {
            return;
        }
    }
}

http_client_request_storage http_client_pool::make_http3_request(const http_client_request_storage& request) {
    std::pmr::vector<http_header_view> headers(resource_);
    auto source_value = request.view(headers);
    std::pmr::string cookie_header(resource_);
    policy_.append_headers(request, headers, cookie_header);

    http_client_request_storage wire(source_value.method_.view(), source_value.target_.view(), resource_);
    wire.set_replay_safe(source_value.replay_safe_);
    for (const auto& header : headers) {
        wire.append_header(header.name(), header.value());
    }
    if (const auto* bytes = source_value.content_.borrowed_bytes()) {
        wire.set_body(bytes->value());
    }
    if (request.upload() != nullptr) {
        wire.bind_upload(*request.upload());
    }
    if (request.is_tunnel()) {
        wire.set_tunnel(request.tunnel_authority(), request.tunnel_protocol());
    }
    if (request.tunnel() != nullptr) {
        wire.bind_tunnel(*request.tunnel());
    }
    return wire;
}

http_client_response_state* http_client_pool::accept_http3_push(std::size_t slot, http3_client_connection& connection,
    std::uint64_t request_id, const http3_message_head& head) noexcept {
    try {
        if (pushes_.size() >= config_.push_.max_queued_pushes_ || active_pushes_ >= config_.push_.max_concurrent_pushes_ ||
            !http_ascii_equals_ignore_case(head.scheme_, "https") ||
            !http_authorities_equal(borrowed_text(std::string_view(head.authority_)),
                borrowed_text(std::string_view(config_.authority_)), 443)) {
            ++rejected_pushes_;
            return nullptr;
        }
        http_client_response response(*this);
        auto& state_value = *response.state_;
        state_value.buffered_limit_ = config_.max_response_bytes_;
        state_value.request_method_ = classify_http_method(head.method_);
        state_value.protocol_version_ = http_protocol_version::http3;
        state_value.connection_index_ = slot;
        state_value.promised_request_.emplace(state_value.resource_);
        auto& request = *state_value.promised_request_;
        request.method_ = head.method_;
        request.scheme_ = head.scheme_;
        request.authority_ = head.authority_;
        request.path_ = head.path_;
        request.headers_.reserve(head.headers_.size());
        for (const auto& field : head.headers_) {
            request.headers_.push_back(http_header::copy_of(field.name(), field.value(), state_value.resource_));
        }
        // Publish only after every borrowed promise field has been copied. If
        // queue allocation fails, the still-unbound local response owns cleanup.
        pushes_.push_back(http_client_push(std::move(response)));
        state_value.transport_ = http_client_response_transport::http3;
        state_value.http3_connection_ = &connection;
        state_value.http3_request_id_ = request_id;
        state_value.request_id_ = request_id;
        state_value.retain_reference();
        ++active_pushes_;
        ++received_pushes_;
        return &state_value;
    } catch (...) {
        ++rejected_pushes_;
        return nullptr;
    }
}

task<void> http_client_pool::execute_http3(std::size_t connection_index, const http_client_request_storage& request,
    const operation_timeout& timeout, stop_token stop_token_value, http_client_response_state& state_value) {
    struct cancellation_guard final {
        client_quic_connections& connections_;
        worker_cancellation_registration<http_client_cancellation_target>& registration_;
        std::uint64_t id_;

        ~cancellation_guard() {
            registration_.reset();
            connections_.unregister_cancellation(id_);
        }
    };

    std::optional<http_client_request_storage> wire;
    wire.emplace(make_http3_request(request));
    auto absolute_deadline = timeout.deadline();
    bool retried_rejected_request = false;
    const auto connection_count = quic_.size();
    if (connection_count == 0) {
        throw http_client_error(http_client_error::code_type::protocol_unavailable, "HTTP/3 client is not configured");
    }

    for (;;) {
        http3_client_connection* connection_value = nullptr;
        std::size_t selected_index = connection_index % connection_count;
        for (;;) {
            for (std::size_t offset = 0; offset < connection_count; ++offset) {
                const auto candidate_index = (connection_index + offset) % connection_count;
                auto& candidate_value = quic_.acquire(candidate_index);
                if (candidate_value.accepting()) {
                    connection_value = &candidate_value;
                    selected_index = candidate_index;
                    break;
                }
            }
            if (connection_value != nullptr) {
                break;
            }
            if (timeout.expired()) {
                throw http_client_error(http_client_error::code_type::timeout, "HTTP/3 connection rotation timed out");
            }
            if (stop_token_value.stop_requested()) {
                throw http_client_error(http_client_error::code_type::cancelled, "HTTP/3 request cancelled");
            }

            worker_timer_registration timer;
            if (const auto remaining = timeout.remaining()) {
                (void)worker_.schedule_timer(timer, worker_timer_deadline_after(*remaining), [this](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        quic_.generation_signal().notify();
                    }
                });
            }
            worker_cancellation_registration registration(cancellation_target_, state_value.cancellation_id_);
            const auto cancellation_id = registration.id();
            quic_.register_cancellation(cancellation_id, nullptr, 0);
            cancellation_guard cancellation{quic_, registration, cancellation_id};
            registration.arm(stop_token_value);
            co_await quic_.generation_signal().wait();
            timer.cancel();
        }

        state_value.connection_index_ = selected_index;
        const auto submission = connection_value->submit(std::move(*wire), state_value, absolute_deadline);
        wire.reset();
        if (submission.outcome_ != http3_client_connection::outcome_type::pending || submission.id_ == 0) {
            throw http_client_error(http3_client_error_code(submission.outcome_), "HTTP/3 request could not be submitted");
        }
        try {
            connection_value->start_if_needed();
        } catch (...) {
            connection_value->request_stop();
            throw;
        }

        http3_client_connection::outcome_type outcome = http3_client_connection::outcome_type::pending;
        std::optional<http3_client_connection::rejected_request_type> rejected;
        {
            worker_cancellation_registration registration(cancellation_target_, state_value.cancellation_id_);
            const auto cancellation_id = registration.id();
            quic_.register_cancellation(cancellation_id, connection_value, submission.id_);
            cancellation_guard cancellation{quic_, registration, cancellation_id};
            registration.arm(stop_token_value);

            co_await connection_value->wait(submission.id_);
            if (state_value.http3_connection_ != connection_value) {
                // The public response may be abandoned and retired by its
                // waiter before this terminal notification is dispatched.
                quic_.generation_signal().notify();
                co_return;
            }
            const auto* result_value = connection_value->result(submission.id_);
            if (result_value == nullptr) {
                std::terminate();
            }
            outcome = result_value->outcome_;
            if (outcome == http3_client_connection::outcome_type::request_rejected) {
                if (!retried_rejected_request && !timeout.expired() && !stop_token_value.stop_requested()) {
                    auto handoff = connection_value->take_rejected_request(submission.id_);
                    if (handoff) {
                        rejected.emplace(std::move(*handoff));
                    }
                }
            } else if (state_value.http3_connection_ == connection_value &&
                       !connection_value->release_response_request(submission.id_)) {
                std::terminate();
            }
        }
        quic_.generation_signal().notify();

        if (rejected) {
            wire.emplace(std::move(rejected->request_));
            absolute_deadline = rejected->deadline_;
            retried_rejected_request = true;
            connection_index = connection_count == 1 ? selected_index : (selected_index + 1) % connection_count;
            if (timeout.expired()) {
                throw http_client_error(http_client_error::code_type::timeout, "HTTP/3 request retry exceeded its deadline");
            }
            if (stop_token_value.stop_requested()) {
                throw http_client_error(http_client_error::code_type::cancelled, "HTTP/3 request cancelled before retry");
            }
            continue;
        }
        if (outcome == http3_client_connection::outcome_type::request_rejected) {
            if (stop_token_value.stop_requested()) {
                throw http_client_error(
                    http_client_error::code_type::cancelled, "HTTP/3 request cancelled after peer rejection");
            }
            if (timeout.expired()) {
                throw http_client_error(
                    http_client_error::code_type::timeout, "HTTP/3 request deadline expired after peer rejection");
            }
            throw http_client_error(
                http_client_error::code_type::protocol_error, "HTTP/3 peer rejected the request as unprocessed");
        }
        co_return;
    }
}

void http_client_pool::reprioritize(http_client_response_state& state_value, http_priority priority) {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (state_value.transport_ == http_client_response_transport::http3 && state_value.http3_connection_ != nullptr) {
        if (!state_value.http3_connection_->reprioritize(state_value.http3_request_id_, priority)) {
            throw http_client_error(http_client_error::code_type::queue_full, "HTTP/3 priority update could not be queued");
        }
        return;
    }
    if (state_value.transport_ != http_client_response_transport::http2 ||
        state_value.connection_index_ >= connections_.size()) {
        throw http_client_error(
            http_client_error::code_type::protocol_unavailable, "priority updates require HTTP/2 or HTTP/3");
    }
    connection(state_value.connection_index_).reprioritize(state_value, priority);
}

std::optional<http_client_push> http_client_pool::next_push() {
    if (!worker_.is_current()) {
        throw std::logic_error("push requires its owner worker");
    }
    if (pushes_.empty()) {
        return std::nullopt;
    }
    auto result_value = std::move(pushes_.front());
    pushes_.pop_front();
    return result_value;
}

void http_client_pool::spawn_request(
    http_client_request_storage request, operation_options options, http_client_response_state& state_value) {
    state_value.buffered_limit_ = config_.max_response_bytes_;
    if (state_value.tunnel_) {
        request.bind_tunnel(*state_value.tunnel_);
    }
    if (state_value.upload_) {
        request.bind_upload(*state_value.upload_);
    }
    background_tasks_.spawn(execute_into(std::move(request), std::move(options), &state_value));
}

task<http_client_tunnel_result> http_client_pool::open_tunnel(
    http_client_request_storage request, http_client_tunnel_config config, operation_options options) {
    require_accepting();
    auto owned_request = std::move(request).into_resource(resource_);
    http_client_response response(*this);
    auto& state_value = *response.state_;
    state_value.tunnel_.emplace(state_value.memory_domain().worker(), state_value.resource_, config);
    state_value.tunnel_->udp_ = owned_request.udp_tunnel();
    state_value.tunnel_->extended_connect_ = !owned_request.tunnel_protocol().empty();
    spawn_request(std::move(owned_request), std::move(options), state_value);
    co_await state_value.await_head("CONNECT handshake failed");
    if (state_value.tunnel_->accepted_) {
        co_return http_client_tunnel_result(http_client_tunnel(std::move(response)));
    }
    co_return http_client_tunnel_result(std::move(response));
}

task<http_client_exchange> http_client_pool::open_request(
    http_client_request_storage request, http_client_upload_config upload, operation_options options) {
    require_accepting();
    auto owned_request = std::move(request).into_resource(resource_);
    http_client_response response(*this);
    auto& state_value = *response.state_;
    state_value.upload_.emplace(state_value.memory_domain().worker(), state_value.resource_, upload);
    state_value.upload_->content_released_ = upload.expectation_ == http_client_request_expectation::none;
    spawn_request(std::move(owned_request), std::move(options), state_value);
    co_return http_client_exchange(std::move(response));
}

task<http_client_response> http_client_pool::execute(http_client_request_storage request, operation_options options) {
    require_accepting();
    // The request owns all data before transport work can outlive the caller.
    auto owned_request = std::move(request).into_resource(resource_);
    http_client_response response(*this);
    auto& state_value = *response.state_;
    spawn_request(std::move(owned_request), std::move(options), state_value);
    co_await state_value.await_head("HTTP client request failed before the response head");
    co_return response;
}

task<void> http_client_pool::execute_into(
    http_client_request_storage request, operation_options options, http_client_response_state* state_value) {
    http_client_response keep_alive(state_value);
    try {
        throw_if_abandoned(*state_value);
        co_await execute_request_into(std::move(request), std::move(options), state_value);
    } catch (...) {
        state_value->failure_ = std::current_exception();
    }
    state_value->complete_ = true;
    if (auto* output = state_value->output()) {
        output->stop();
    }
    state_value->head_signal_.notify();
    state_value->data_signal_.notify();
}

void http_client_pool::require_accepting() const {
    // join() retires background_tasks_, which then rejects every spawn.
    if (background_joined_) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP client pool is retired");
    }
}

task<void> http_client_pool::execute_request_into(
    http_client_request_storage request, operation_options options, http_client_response_state* state_value) {
    throw_if_abandoned(*state_value);
    const operation_timeout timeout(options.timeout_.has_value() ? options.timeout_ : config_.request_timeout_);
    const auto acquire_timeout = timeout.constrained_by(config_.acquire_timeout_);
    if (requests_buffered_ >= config_.max_buffered_requests_) {
        ++failed_requests_;
        throw http_client_error(http_client_error::code_type::queue_full, "http client request buffer is full");
    }
    ++requests_buffered_;
    std::size_t index = 0;
    try {
        index = co_await slots_.acquire(timeout, config_.acquire_timeout_, options.stop_token_);
    } catch (...) {
        --requests_buffered_;
        ++failed_requests_;
        throw;
    }
    --requests_buffered_;
    ++requests_in_flight_;
    struct lease_type final {
        http_client_pool& pool_;
        std::size_t index_;
        http_client_connection* discard_{nullptr};
        ~lease_type() {
            if (discard_ != nullptr) {
                discard_->close();
            }
            pool_.slots_.release(index_);
        }
    } lease{*this, index};
    bool discard_connection = false;
    http_client_connection* tcp_connection = nullptr;
    try {
        throw_if_abandoned(*state_value);
        if (config_.protocol_ == http_client_protocol::http3_only) {
            state_value->connection_index_ = index % quic_.size();
            // The connection records this request's result when it publishes
            // the terminal response; this driver accounts only what it ends.
            state_value->pool_request_ = &request;
            struct pool_request_guard final {
                http_client_response_state& state_;
                ~pool_request_guard() {
                    state_.pool_request_ = nullptr;
                }
            } pool_request{*state_value};
            std::exception_ptr failure;
            try {
                co_await execute_http3(state_value->connection_index_, request, timeout, options.stop_token_, *state_value);
            } catch (...) {
                failure = std::current_exception();
            }
            finish_http3_request(*state_value, failure == nullptr && !state_value->failure_);
            if (failure != nullptr) {
                std::rethrow_exception(failure);
            }
            co_return;
        }
        tcp_connection = &connection(index);
        state_value->connection_index_ = tcp_connection->index();
        co_await execute_tcp(*tcp_connection, request, timeout, acquire_timeout, options.stop_token_, *state_value,
            discard_connection);
        policy_.retain_response_cookies(request, state_value->headers_);
        --requests_in_flight_;
        ++completed_requests_;
    } catch (...) {
        if (config_.protocol_ != http_client_protocol::http3_only) {
            --requests_in_flight_;
            ++failed_requests_;
        }
        if (discard_connection) {
            lease.discard_ = tcp_connection;
        }
        throw;
    }
}

void http_client_pool::finish_http3_request(http_client_response_state& state_value, bool completed) noexcept {
    const auto* request = std::exchange(state_value.pool_request_, nullptr);
    if (request == nullptr) {
        return;
    }
    --requests_in_flight_;
    if (!completed) {
        ++failed_requests_;
        return;
    }
    ++completed_requests_;
    if (state_value.head_ready_) {
        try {
            policy_.retain_response_cookies(*request, state_value.headers_);
        } catch (...) {
            // Cookie retention is best effort after the response completed;
            // a full or failing store keeps its previous cookies.
        }
    }
}

task<void> http_client_pool::execute_tcp(http_client_connection& connection_value,
    const http_client_request_storage& request, const operation_timeout& timeout,
    const operation_timeout& acquire_timeout, stop_token stop_token_value, http_client_response_state& state_value,
    bool& discard_connection) {
    discard_connection = false;
    // A cancelled HTTP/1 producer still owns its SSL stream until its
    // serialized exchange unwinds. Join that exchange before reconnecting,
    // including when negotiated HTTP/2-capacity leases share the pool slot.
    if (connection_value.protocol_ == http_client_connection::protocol_type::http2) {
        // This lease shares a multiplexed session with background drivers and
        // possibly other requests; a request-local failure must not discard it.
        discard_connection = false;
        co_await connection_value.execute_http2(request, timeout, stop_token_value, state_value);
        co_return;
    }
    // A negotiated HTTP/1 connection can have several operations that already
    // hold outer HTTP/2-capacity slots. Waiting for this connection's single
    // exchange slot does not own its socket: a timeout/cancellation here must
    // not close the exchange currently using it.
    discard_connection = false;
    const bool must_buffer = connection_value.http1_operations_ != 0;
    if (must_buffer && requests_buffered_ >= config_.max_buffered_requests_) {
        throw http_client_error(http_client_error::code_type::queue_full, "HTTP client request buffer is full");
    }
    bool retry_as_http2 = false;
    {
        ++connection_value.http1_operations_;
        if (must_buffer) {
            --requests_in_flight_;
            ++requests_buffered_;
        }
        struct http1_operation final {
            http_client_pool& pool_;
            http_client_connection& connection_;
            bool buffered_;
            ~http1_operation() {
                if (buffered_) {
                    --pool_.requests_buffered_;
                    ++pool_.requests_in_flight_;
                }
                if (connection_.http1_operations_ == 0) {
                    std::terminate();
                }
                --connection_.http1_operations_;
            }
        } operation{*this, connection_value, must_buffer};
        const auto slot =
            co_await acquire_http1_exchange_slot(connection_value.http1_scheduler_, acquire_timeout, stop_token_value);
        if (operation.buffered_) {
            --requests_buffered_;
            ++requests_in_flight_;
            operation.buffered_ = false;
        }
        struct http1_release final {
            pool_lease_scheduler& scheduler_;
            std::size_t slot_;
            ~http1_release() {
                (void)scheduler_.release(slot_);
            }
        } release{connection_value.http1_scheduler_, slot};

        throw_if_abandoned(state_value);
        // A previous HTTP/1 exchange may have closed this connection while this
        // request waited for the serial exchange slot. Reconnect only after
        // acquiring that slot, so the connection cannot be replaced underneath
        // an active HTTP/1 exchange. Every failure below closes the connection
        // before the slot is released; pool handoff resumes the next waiter
        // synchronously.
        try {
            co_await connection_value.ensure_connected(timeout, acquire_timeout, stop_token_value, state_value);
            if (connection_value.protocol_ == http_client_connection::protocol_type::http2) {
                retry_as_http2 = true;
            } else {
                http1_client_exchange exchange(*this, connection_value);
                co_await exchange.run(request, timeout, stop_token_value, state_value);
            }
        } catch (...) {
            connection_value.close();
            throw;
        }
    }
    if (retry_as_http2) {
        co_await connection_value.execute_http2(request, timeout, stop_token_value, state_value);
    }
}

void http_client_pool::abandon_response(http_client_response_state& state_value) noexcept {
    if (state_value.abandoned_) {
        return;
    }
    state_value.abandoned_ = true;
    switch (state_value.transport_) {
        case http_client_response_transport::http2:
            if (state_value.connection_index_ < connections_.size() && state_value.request_id_ != 0) {
                connection(state_value.connection_index_)
                    .cancel_http2_stream(state_value.request_id_, client_abort_reason::cancelled);
            }
            break;
        case http_client_response_transport::http3:
            if (state_value.http3_connection_ == nullptr || state_value.http3_request_id_ == 0) {
                std::terminate();
            }
            state_value.http3_connection_->abandon_response(state_value.http3_request_id_);
            break;
        case http_client_response_transport::unassigned:
        case http_client_response_transport::http1:
            if (state_value.cancellation_id_ != 0) {
                cancel_operation_by_id(state_value.cancellation_id_);
            } else if (state_value.connection_index_ < connections_.size() &&
                       connection(state_value.connection_index_).http1_response_ == &state_value) {
                connection(state_value.connection_index_).close();
            }
            break;
    }
    if (auto* output = state_value.output()) {
        output->stop();
    }
    state_value.space_signal_.notify();
}

void http_client_pool::release_response_data(http_client_response_state& state_value) noexcept {
    if (state_value.connection_index_ < connections_.size()) {
        connection(state_value.connection_index_).release_response_data(state_value);
    } else {
        state_value.http2_data_credit_.reset();
    }
}

}  // namespace ruvia::detail
