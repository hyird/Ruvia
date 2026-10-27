#include "client/http_client_pool.h"

#include <cstdint>
#include <exception>
#include <optional>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/http/http_known_method.h"

#include "client/client_tls.h"
#include "client/http1_client_exchange.h"
#include "client/http_client_connection.h"
#include "client/http_client_response_state.h"
#include "client/http_client_result_budget.h"

namespace ruvia::detail {
namespace {
void throw_if_abandoned(const http_client_response_state& state_value) {
    if (state_value.abandoned_) {
        throw http_client_error(
            http_client_error::code_type::cancelled, "HTTP request abandoned before transport admission");
    }
}

[[nodiscard]] bool can_retry_reused_http1_request(const std::exception_ptr& failure,
    const http_client_request_storage& request,
    const http_client_response_state& response) noexcept {
    if (request.upload() != nullptr || request.is_tunnel() ||
        !is_http_method_idempotent(request.method()) || response.head_ready_ ||
        !response.informational_.empty() || response.abandoned_) {
        return false;
    }
    try {
        std::rethrow_exception(failure);
    } catch (const http_client_error& error) {
        return error.code() == http_client_error::code_type::io_error;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] std::size_t http_client_scheduler_slots(const http_client_config_storage& config) noexcept {
    if (config.protocol_ == http_client_protocol::http1_only) {
        return config.connection_count_;
    }
    return config.connection_count_ * config.max_concurrent_http2_streams_per_connection_;
}

[[noreturn]] void throw_http1_acquire_failure(worker_wait_status status) {
    if (status == worker_wait_status::timed_out) {
        throw http_client_error(http_client_error::code_type::timeout, "HTTP/1 connection acquire timed out");
    }
    if (status == worker_wait_status::cancelled) {
        throw http_client_error(http_client_error::code_type::cancelled, "HTTP/1 request cancelled");
    }
    if (status == worker_wait_status::closed) {
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
      result_budget_domain_(std::move(result_budget_domain)),
      tls_context_(asio::ssl::context::tls_client),
      slots_(*this, http_client_scheduler_slots(config_), worker_, resource_),
      cancellation_target_(make_worker_cancellation_target(*this, worker_)),
      background_tasks_(worker_, {.resource_ = resource_}),
      connections_(resource_) {
    if (config_.scheme_ == http_scheme::https) {
        configure_client_tls_context(*tls_context_.native_handle(), config_.tls_);
    }
    connections_.reserve(config_.connection_count_);
    for (std::size_t index = 0; index < config_.connection_count_; ++index) {
        connections_.push_back(make_pmr_object<http_client_connection>(resource_, *this, index));
    }
    response_memory_ = response_storage == http_client_response_storage::worker_resource
                           ? http_client_response_memory_domain::create(worker_, result_budget_domain_, *resource_)
                           : http_client_response_memory_domain::create_response_owned(worker_, result_budget_domain_);
}

http_client_pool::~http_client_pool() {
    close_now();
    cancellation_target_->detach(*this);
}

void http_client_pool::close_now() noexcept {
    if (!slots_.close()) {
        return;
    }
    // slots_.close() retires queued waiters and connection shutdown aborts
    // every connect and exchange; a transferred slot fails at admission.
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
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

http_client_stats http_client_pool::stats() const noexcept {
    return {.buffered_requests_ = requests_buffered_,
        .in_flight_requests_ = requests_in_flight_,
        .completed_requests_ = completed_requests_,
        .failed_requests_ = failed_requests_,
        .plaintext_bytes_sent_ = wire_counters_.sent_,
        .plaintext_bytes_received_ = wire_counters_.received_,
        .idle_connections_with_pending_data_ = idle_connections_with_pending_data_,
        .dropped_advertisements_ = advertisements_.dropped(),
        .received_pushes_ = received_pushes_,
        .rejected_pushes_ = rejected_pushes_};
}

void http_client_pool::cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id == 0) {
        return;
    }
    for (auto& connection_value : connections_) {
        if (connection_value->cancel_by_id(cancellation_id)) {
            return;
        }
    }
}

void http_client_pool::reprioritize(http_client_response_state& state_value, http_priority priority) {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (state_value.transport_ != http_client_response_transport::http2 ||
        state_value.connection_index_ >= connections_.size()) {
        throw http_client_error(
            http_client_error::code_type::protocol_unavailable, "priority updates require HTTP/2");
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
        co_await execute_request_into(std::move(request), std::move(options), state_value);
    } catch (...) {
        state_value->upload_only_failure_ =
            state_value->complete_ && state_value->upload_ && state_value->failure_ == nullptr;
        state_value->discard_failed_encoded_response();
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

http_client_connection& http_client_pool::connection_for_request(std::size_t index) noexcept {
    auto& preferred = connection(index);
    if (preferred.protocol_ != http_client_connection::protocol_type::http2 ||
        preferred.available_http2_request_streams() != 0) {
        return preferred;
    }
    for (std::size_t offset = 1; offset < connections_.size(); ++offset) {
        auto& candidate = connection(index + offset);
        if (candidate.protocol_ == http_client_connection::protocol_type::http2 &&
            candidate.available_http2_request_streams() != 0) {
            return candidate;
        }
    }
    return preferred;
}

task<void> http_client_pool::execute_request_into(
    http_client_request_storage request, operation_options options, http_client_response_state* state_value) {
    try {
        throw_if_abandoned(*state_value);
    } catch (...) {
        ++failed_requests_;
        throw;
    }
    const operation_timeout timeout(options.timeout_.has_value() ? options.timeout_ : config_.request_timeout_);
    const auto acquire_timeout = timeout.constrained_by(config_.acquire_timeout_);
    if (requests_buffered_ >= config_.max_buffered_requests_) {
        ++failed_requests_;
        throw http_client_error(http_client_error::code_type::queue_full, "HTTP client request buffer is full");
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
    const slot_pool_type::lease_guard lease(slots_, index);
    http_client_connection* tcp_connection = nullptr;
    try {
        throw_if_abandoned(*state_value);
        tcp_connection = &connection_for_request(index);
        state_value->connection_index_ = tcp_connection->index();
        co_await execute_tcp(*tcp_connection, request, timeout, acquire_timeout, options.stop_token_, *state_value);
        --requests_in_flight_;
        ++completed_requests_;
    } catch (...) {
        --requests_in_flight_;
        ++failed_requests_;
        throw;
    }
}

task<void> http_client_pool::execute_tcp(http_client_connection& connection_value,
    const http_client_request_storage& request, const operation_timeout& timeout,
    const operation_timeout& acquire_timeout, stop_token stop_token_value, http_client_response_state& state_value) {
    std::optional<std::uint64_t> connected_http1_generation;
    // A cancelled HTTP/1 producer still owns its SSL stream until its
    // serialized exchange unwinds. Join that exchange before reconnecting,
    // including when negotiated HTTP/2-capacity leases share the pool slot.
    if (connection_value.protocol_ == http_client_connection::protocol_type::http2) {
        // This lease shares a multiplexed session with background drivers and
        // possibly other requests; a request-local failure must not discard it.
        // A session that ended and was replaced by an HTTP/1.1 connection has
        // sent nothing yet: the request continues as an HTTP/1 exchange below.
        if (co_await connection_value.execute_http2(request, timeout, stop_token_value, state_value)) {
            co_return;
        }
        connected_http1_generation = connection_value.generation_;
    }
    // A negotiated HTTP/1 connection can have several operations that already
    // hold outer HTTP/2-capacity slots. Waiting for this connection's single
    // exchange slot does not own its socket: a timeout/cancellation here must
    // not close the exchange currently using it.
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
        const auto acquired =
            co_await connection_value.http1_scheduler_.acquire(acquire_timeout.remaining(), stop_token_value);
        if (!acquired.has_value()) {
            throw_http1_acquire_failure(acquired.status());
        }
        const auto slot = acquired.value();
        if (operation.buffered_) {
            --requests_buffered_;
            ++requests_in_flight_;
            operation.buffered_ = false;
        }
        struct http1_release final {
            pool_lease_scheduler& scheduler_;
            std::size_t slot_;
            ~http1_release() {
                scheduler_.release(slot_);
            }
        } release{connection_value.http1_scheduler_, slot};

        throw_if_abandoned(state_value);
        if (connection_value.protocol_ == http_client_connection::protocol_type::http2) {
            retry_as_http2 = true;
        } else {
            // A previous HTTP/1 exchange may have closed this connection while this
            // request waited for the serial exchange slot. Reconnect only after
            // acquiring that slot, so the connection cannot be replaced underneath
            // an active HTTP/1 exchange. Every final failure below closes the
            // connection before the slot is released; pool handoff resumes the next
            // waiter synchronously.
            try {
                // The HTTP/2 fallback already connected and negotiated HTTP/1.
                // Do not idle-probe this fresh TLS stream before its first response:
                // session tickets can look like unsolicited response bytes. A connect
                // or exchange while waiting for this slot invalidates the snapshot.
                bool reused_http1 = false;
                if (!connection_value.connected_ ||
                    connected_http1_generation != connection_value.generation_) {
                    reused_http1 = co_await connection_value.ensure_connected(
                        timeout, acquire_timeout, stop_token_value, state_value);
                }
                if (connection_value.protocol_ == http_client_connection::protocol_type::http2) {
                    retry_as_http2 = true;
                } else {
                    bool retried = false;
                    for (;;) {
                        std::exception_ptr failure;
                        try {
                            http1_client_exchange exchange(*this, connection_value);
                            co_await exchange.run(request, timeout, stop_token_value, state_value);
                        } catch (...) {
                            failure = std::current_exception();
                        }
                        if (failure == nullptr) {
                            break;
                        }
                        if (retried || !reused_http1 ||
                            !can_retry_reused_http1_request(failure, request, state_value)) {
                            std::rethrow_exception(failure);
                        }
                        // RFC 9110 section 9.2.2 permits automatically retrying
                        // an idempotent request. Limit that recovery to one failed
                        // reused transport and a response head not yet published.
                        connection_value.close();
                        connection_value.retain_idle_buffers();
                        retried = true;
                        reused_http1 = false;
                        (void)co_await connection_value.ensure_connected(
                            timeout, acquire_timeout, stop_token_value, state_value);
                        if (connection_value.protocol_ ==
                            http_client_connection::protocol_type::http2) {
                            retry_as_http2 = true;
                            break;
                        }
                    }
                }
            } catch (...) {
                if (connection_value.protocol_ != http_client_connection::protocol_type::http2) {
                    connection_value.close();
                    // exchange.run() has joined its child operations, so no I/O
                    // still borrows these failed exchange buffers.
                    connection_value.retain_idle_buffers();
                }
                throw;
            }
        }
    }
    if (retry_as_http2 &&
        !co_await connection_value.execute_http2(request, timeout, stop_token_value, state_value)) {
        // One protocol change per request: the HTTP/2 session negotiated above
        // ended before this request could use it and its replacement speaks
        // HTTP/1.1 again.
        throw http_client_error(
            http_client_error::code_type::protocol_unavailable, "upstream renegotiated the protocol repeatedly");
    }
}

void http_client_pool::abandon_response(http_client_response_state& state_value, http2_error_code error) noexcept {
    if (state_value.abandoned_) {
        return;
    }
    state_value.abandoned_ = true;
    switch (state_value.transport_) {
        case http_client_response_transport::http2:
            if (state_value.connection_index_ < connections_.size()) {
                auto& conn = connection(state_value.connection_index_);
                if (conn.generation_ == state_value.connection_generation_ &&
                    error == http2_error_code::protocol_error && state_value.stream_id_ != 0) {
                    conn.http2_.submit_reset(static_cast<std::uint32_t>(state_value.stream_id_), error);
                }
                if (state_value.request_id_ != 0) {
                    conn.cancel_http2_stream(state_value.request_id_, client_abort_reason::cancelled, error);
                }
                // A retry may be waiting for the old session without a
                // registered stream. Wake it to observe this abandonment.
                conn.http2_.state_signal().notify();
            }
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
