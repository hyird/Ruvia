#include "client/http_client_pool.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/connect.hpp>
#include <asio/ssl/error.hpp>
#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"

#include "client/client_transport.h"
#include "client/http_client_response_state.h"
#include "client/http_client_result_budget.h"
#include "http3/http3_client_connection.h"

namespace ruvia::detail {
namespace {

constexpr std::size_t connection_read_buffer_initial_bytes = std::size_t{16} * 1024;

client_alpn_mode select_client_alpn_mode(http_client_protocol protocol) noexcept {
    switch (protocol) {
        case http_client_protocol::negotiate:
            return client_alpn_mode::negotiate;
        case http_client_protocol::http1_only:
            return client_alpn_mode::http11;
        case http_client_protocol::http2_only:
            return client_alpn_mode::http2;
        case http_client_protocol::http3_only:
            break;
    }
    std::terminate();
}

std::size_t http_client_scheduler_slots(const http_client_config_storage& config) noexcept {
    if (config.protocol_ == http_client_protocol::http1_only) {
        return config.connection_count_;
    }
    if (config.protocol_ == http_client_protocol::http3_only) {
        return config.connection_count_ * client_quic_connections::requests_per_connection;
    }
    return config.connection_count_ * config.max_concurrent_http2_streams_per_connection_;
}

[[nodiscard]] http_client_error::code_type http3_client_error_code(
    http3_client_connection::outcome_type outcome) noexcept {
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
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain) {
    if (!result_budget_domain) {
        throw std::invalid_argument("HTTP client result budget domain must not be null");
    }
    return result_budget_domain;
}

}  // namespace

static_assert(worker_cancellation_post_is_inline<http_client_cancellation_target>);

http_client_pool::connection_type::connection_type(asio::io_context& io_context, asio::ssl::context& tls_context,
    const worker_handle& worker_value, const http_client_config_storage& config, client_wire_counters& counters,
    std::pmr::memory_resource* resource)
    : transport_(io_context, tls_context, worker_value, config, counters, resource),
      read_buffer_(pmr_resource_or_default(resource)),
      write_buffer_(pmr_resource_or_default(resource)),
      http2_(nullptr, pmr_object_deleter<::ruvia::http2_connection>{pmr_resource_or_default(resource)}),
      http2_runtime_(make_pmr_object<http2_runtime_type>(resource, worker_value, resource)) {
    read_buffer_.reserve(connection_read_buffer_initial_bytes);
}

http_client_pool::connection_type::~connection_type() = default;
http_client_pool::connection_type::connection_type(connection_type&&) noexcept = default;

http_client_pool::http_client_pool(asio::io_context& io_context, const worker_handle& worker_value,
    http_client_config_storage config, http_client_result_budget_config result_budget,
    std::pmr::memory_resource* resource)
    : http_client_pool(io_context, worker_value, std::move(config),
          std::make_shared<http_client_result_budget_domain>(result_budget), resource) {}

http_client_pool::http_client_pool(asio::io_context& io_context, const worker_handle& worker_value,
    http_client_config_storage config,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain,
    std::pmr::memory_resource* resource)
    : io_context_(io_context),
      worker_(worker_value),
      resource_(pmr_resource_or_default(resource)),
      config_(std::move(config)),
      policy_(config_, resource_),
      advertisements_(worker_, config_.advertisements_, resource_),
      pushes_(resource_),
      result_budget_domain_(require_http_client_result_budget_domain(result_budget_domain)),
      tls_context_(asio::ssl::context::tls_client),
      connections_(resource_),
      scheduler_(http_client_scheduler_slots(config_), worker_, resource_),
      background_tasks_(worker_, {.resource_ = resource_}),

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
            configure_client_tls_context(*tls_context_.native_handle(), config_.transport_.view());
        }
        connections_.reserve(config_.connection_count_);
        for (std::size_t i = 0; i < config_.connection_count_; ++i) {
            connections_.emplace_back(io_context_, tls_context_, worker_, config_, wire_counters_, resource_);
        }
    }
    cancellation_target_ = make_worker_cancellation_target(*this, worker_);
    response_memory_ = http_client_response_memory_domain::create(worker_, result_budget_domain_);
}

http_client_pool::~http_client_pool() {
    close_now();
}

http_client_pool::lease_type::~lease_type() {
    if (discard_) {
        pool_.close(connection());
    }
    pool_.release(index_);
}

void http_client_pool::close(connection_type& connection) noexcept {
    connection.transport_.stop_output();
    auto& runtime = *connection.http2_runtime_;
    if (runtime.running_) {
        // A multiplexed connection owns background reader/writer tasks. Route
        // every terminal close through their shared failure transition so both
        // drivers and all pending streams are woken before join().
        fail_http2_session(
            connection, runtime.generation_, std::make_error_code(std::errc::operation_canceled));
        return;
    }
    connection.transport_.close();
    connection.connected_ = false;
    connection.protocol_ = wire_protocol_type::unknown;
    if (runtime.session_tasks_ == 0) {
        connection.http2_.reset();
        runtime.running_ = false;
        runtime.draining_ = false;
        runtime.failed_ = false;
    }
    connection.read_buffer_.clear();
    connection.write_buffer_.clear();
}

void http_client_pool::close_now() noexcept {
    cancellation_target_->detach(*this);
    if (!scheduler_.close()) {
        return;
    }
    quic_.request_stop();
    background_tasks_.request_stop();
    for (auto& connection : connections_) {
        connection.transport_.abort_output(client_abort_reason::closing);
        auto& runtime = *connection.http2_runtime_;
        (void)runtime.connect_scheduler_.close();
        (void)runtime.http1_scheduler_.close();
        if (runtime.running_) {
            fail_http2_session(connection, runtime.generation_,
                std::make_error_code(std::errc::operation_canceled));
        } else {
            close(connection);
        }
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

ruvia::quic_path_migration http_client_pool::start_quic_path_migration(const asio::ip::udp::endpoint& endpoint) {
    if (background_joined_) {
        return {.status_ = ruvia::quic_migration_status::rejected};
    }
    return quic_.start_path_migration(endpoint);
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

std::uint16_t http_client_pool::port() const noexcept {
    return http_client_port(config_);
}

task<std::size_t> http_client_pool::acquire(const ruvia::operation_timeout& timeout, stop_token stop_token_value) {
    auto result_value = co_await scheduler_.acquire(
        timeout.constrained_by(config_.acquire_timeout_).remaining(), std::move(stop_token_value));
    switch (result_value.status()) {
        case ruvia::pool_waiter_result::status_type::acquired:
            co_return result_value.index();
        case ruvia::pool_waiter_result::status_type::timed_out:
            throw http_client_error(
                http_client_error::code_type::timeout, "http client connection pool acquire timed out");
        case ruvia::pool_waiter_result::status_type::cancelled:
            throw http_client_error(http_client_error::code_type::cancelled, "http client request cancelled");
        case ruvia::pool_waiter_result::status_type::closed:
            throw http_client_error(http_client_error::code_type::closing, "http client pool is closing");
    }
    std::terminate();
}

void http_client_pool::release(std::size_t index) noexcept {
    const auto status = scheduler_.release(index);
    if (status == pool_lease_release_status::invalid_slot ||
        status == pool_lease_release_status::already_released) {
        std::terminate();
    }
}

void http_client_pool::cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id == 0) {
        return;
    }
    if (quic_.cancel(cancellation_id)) {
        return;
    }
    for (std::size_t index = 0; index < connections_.size(); ++index) {
        auto& connection = connections_[index];
        if (connection.cancellation_id_ == cancellation_id) {
            connection.cancellation_id_ = 0;
            cancel_operation(index, connection.generation_, client_abort_reason::cancelled);
            return;
        }
        auto& runtime = *connection.http2_runtime_;
        if (runtime.state_cancellation_id_ == cancellation_id) {
            runtime.state_signal_.notify();
            return;
        }
        const auto pending = std::ranges::find_if(
            runtime.pending_, [cancellation_id](const http2_pending_stream_type* stream) {
                return stream->cancellation_id_ == cancellation_id;
            });
        if (pending != runtime.pending_.end()) {
            (*pending)->cancellation_id_ = 0;
            cancel_http2_stream(connection, (*pending)->request_id_, client_abort_reason::cancelled);
            return;
        }
    }
}

void http_client_pool::cancel_operation(
    std::size_t index, std::uint64_t generation, client_abort_reason reason) noexcept {
    if (connections_.empty()) {
        return;
    }
    auto& connection = connections_[index % connections_.size()];
    if (connection.generation_ != generation) {
        return;
    }
    connection.transport_.cancel(reason);
    connection.connected_ = false;
    connection.protocol_ = wire_protocol_type::unknown;
    connection.http2_runtime_->write_signal_.notify();
    connection.http2_runtime_->state_signal_.notify();
}

task<void> http_client_pool::ensure_connected(connection_type& connection,
    const ruvia::operation_timeout& operation_timeout_value, const ruvia::operation_timeout& acquire_timeout,
    stop_token stop_token_value) {
    auto& runtime = *connection.http2_runtime_;
    auto acquired =
        co_await runtime.connect_scheduler_.acquire(acquire_timeout.remaining(), stop_token_value);
    switch (acquired.status()) {
        case ruvia::pool_waiter_result::status_type::acquired:
            break;
        case ruvia::pool_waiter_result::status_type::timed_out:
            throw http_client_error(
                http_client_error::code_type::timeout, "HTTP client connect wait timed out");
        case ruvia::pool_waiter_result::status_type::cancelled:
            throw http_client_error(
                http_client_error::code_type::cancelled, "HTTP client connect wait cancelled");
        case ruvia::pool_waiter_result::status_type::closed:
            throw http_client_error(http_client_error::code_type::closing, "HTTP client pool is closing");
        default:
            std::terminate();
    }
    const auto connect_slot = acquired.index();
    struct connect_lease final {
        pool_lease_scheduler& scheduler_;
        std::size_t slot_;
        ~connect_lease() {
            (void)scheduler_.release(slot_);
        }
    } connect_lease_value{runtime.connect_scheduler_, connect_slot};
    if (connection.connected_) {
        co_return;
    }
    connection.transport_.reset_abort();
    ++connection.generation_;
    if (connection.generation_ == 0) {
        ++connection.generation_;
    }
    struct connect_cancellation_generation final {
        connection_type& connection_;
        ~connect_cancellation_generation() {
            ++connection_.generation_;
        }
    } cancellation_generation{connection};
    worker_cancellation_registration cancellation(cancellation_target_, connection.cancellation_id_);
    cancellation.arm(stop_token_value);
    while (runtime.session_tasks_ != 0 || !runtime.pending_.empty()) {
        connection.transport_.throw_if_aborted();
        co_await runtime.state_signal_.wait();
    }
    connection.transport_.throw_if_aborted();
    connection.transport_.prepare_reconnect(tls_context_);
    runtime.connecting_ = true;
    struct connect_guard final {
        http2_runtime_type& runtime_;
        ~connect_guard() {
            runtime_.connecting_ = false;
            runtime_.state_signal_.notify();
        }
    } connect_guard_value{runtime};
    connection.http2_.reset();
    runtime.running_ = false;
    runtime.draining_ = false;
    runtime.failed_ = false;
    const auto timeout = operation_timeout_value.constrained_by(config_.connect_timeout_);
    client_port_text_buffer_type port_buffer{};
    const auto port = format_client_port(http_client_port(config_), port_buffer);
    if (!connection.transport_.arm_deadline(timeout, client_deadline_kind::resolve)) {
        throw http_client_error(http_client_error::code_type::timeout, "http client resolve timed out");
    }
    auto resolve = co_await ruvia::async_asio<asio::ip::tcp::resolver::results_type>(
        [&connection, this, port](auto handler) mutable {
            connection.transport_.resolver().async_resolve(config_.host_, port, std::move(handler));
        });
    const bool resolve_timed_out = connection.transport_.clear_deadline() || timeout.expired();
    connection.transport_.throw_if_aborted();
    if (resolve_timed_out) {
        throw http_client_error(http_client_error::code_type::timeout, "http client resolve timed out");
    }
    if (resolve.error_code()) {
        throw http_client_error(http_client_error::code_type::resolve_failed, resolve.error_code().message());
    }
    auto endpoints = std::move(resolve).take_result();

    if (!connection.transport_.arm_deadline(timeout, client_deadline_kind::socket)) {
        throw http_client_error(http_client_error::code_type::timeout, "http client connect timed out");
    }
    auto connected = co_await ruvia::async_asio([&connection, &endpoints](auto handler) mutable {
        asio::async_connect(connection.transport_.stream().lowest_layer(), endpoints, std::move(handler));
    });
    const bool connect_timed_out = connection.transport_.clear_deadline() || timeout.expired();
    connection.transport_.throw_if_aborted();
    if (connect_timed_out) {
        throw http_client_error(http_client_error::code_type::timeout, "http client connect timed out");
    }
    if (connected.error_code()) {
        throw http_client_error(
            http_client_error::code_type::connect_failed, connected.error_code().message());
    }
    const auto transport = config_.transport_.view();
    ruvia::apply_tcp_socket_policies(
        connection.transport_.stream().next_layer(), transport.tcp_no_delay_, transport.tcp_keep_alive_);

    if (config_.scheme_ == http_scheme::https) {
        connection.transport_.mark_tls_started();
        const auto tls_setup = prepare_client_tls_stream(
            connection.transport_.stream(), config_.host_, transport, select_client_alpn_mode(config_.protocol_));
        if (tls_setup != client_tls_setup_error::none) {
            throw http_client_error(
                http_client_error::code_type::tls_failed, client_tls_setup_error_message(tls_setup));
        }
        if (!connection.transport_.arm_deadline(timeout, client_deadline_kind::socket)) {
            throw http_client_error(
                http_client_error::code_type::timeout, "http client TLS handshake timed out");
        }
        auto handshake = co_await ruvia::async_asio([&connection](auto handler) mutable {
            connection.transport_.stream().async_handshake(asio::ssl::stream_base::client, std::move(handler));
        });
        const bool handshake_timed_out = connection.transport_.clear_deadline() || timeout.expired();
        connection.transport_.throw_if_aborted();
        if (handshake_timed_out) {
            throw http_client_error(
                http_client_error::code_type::timeout, "http client TLS handshake timed out");
        }
        if (handshake.error_code()) {
            throw http_client_error(
                http_client_error::code_type::tls_failed, handshake.error_code().message());
        }
        const auto alpn = selected_client_alpn(connection.transport_.stream().native_handle());
        if (config_.protocol_ == http_client_protocol::http2_only && alpn != "h2") {
            throw http_client_error(
                http_client_error::code_type::protocol_unavailable, "upstream did not negotiate HTTP/2");
        }
        connection.protocol_ = alpn == "h2" ? wire_protocol_type::http2 : wire_protocol_type::http1;
    } else {
        connection.protocol_ = config_.protocol_ == http_client_protocol::http2_only
                                   ? wire_protocol_type::http2
                                   : wire_protocol_type::http1;
    }
    connection.connected_ = true;
    if (connection.protocol_ == wire_protocol_type::http2) {
        co_await initialize_http2(connection, timeout);
    }
}

http_client_request_storage http_client_pool::make_http3_request(
    const http_client_request_storage& request) {
    std::pmr::vector<http_header_view> headers(resource_);
    auto source_value = http_client_request_storage_access::view(request, headers);
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
        const auto origin = http_origin_view::https({.host_ = config_.host_, .port_ = config_.port_});
        const auto authority = make_http_origin_authority(origin, resource_);
        if (pushes_.size() >= config_.push_.max_queued_pushes_ || active_pushes_ >= config_.push_.max_concurrent_pushes_ ||
            !http_ascii_equals_ignore_case(head.scheme_, "https") ||
            !http_authorities_equal(borrowed_text(std::string_view(head.authority_)), borrowed_text(std::string_view(authority)), 443)) {
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
            request.headers_.push_back(http_header::copy_of(field.name_, field.value_, state_value.resource_));
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

task<void> http_client_pool::execute_http3(std::size_t connection_index,
    const http_client_request_storage& request, const ruvia::operation_timeout& timeout,
    stop_token stop_token_value, http_client_response& response) {
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
        throw http_client_error(
            http_client_error::code_type::protocol_unavailable, "HTTP/3 client is not configured");
    }

    for (;;) {
        http3_client_connection* connection = nullptr;
        std::size_t selected_index = connection_index % connection_count;
        for (;;) {
            for (std::size_t offset = 0; offset < connection_count; ++offset) {
                const auto candidate_index = (connection_index + offset) % connection_count;
                auto& candidate_value = quic_.acquire(candidate_index);
                if (candidate_value.accepting()) {
                    connection = &candidate_value;
                    selected_index = candidate_index;
                    break;
                }
            }
            if (connection != nullptr) {
                break;
            }
            if (timeout.expired()) {
                throw http_client_error(
                    http_client_error::code_type::timeout, "HTTP/3 connection rotation timed out");
            }
            if (stop_token_value.stop_requested()) {
                throw http_client_error(
                    http_client_error::code_type::cancelled, "HTTP/3 request cancelled");
            }

            worker_timer_registration timer;
            if (const auto remaining = timeout.remaining()) {
                (worker_).schedule_timer(timer, worker_timer_deadline_after(*remaining), [this](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        quic_.generation_signal().notify();
                    }
                });
            }
            worker_cancellation_registration registration(cancellation_target_, response.state_->cancellation_id_);
            const auto cancellation_id = registration.id();
            quic_.register_cancellation(cancellation_id, nullptr, 0);
            cancellation_guard cancellation{quic_, registration, cancellation_id};
            registration.arm(stop_token_value);
            co_await quic_.generation_signal().wait();
            timer.cancel();
        }

        response.state_->connection_index_ = selected_index;
        const auto submission = connection->submit(std::move(*wire), *response.state_, absolute_deadline);
        wire.reset();
        if (submission.outcome_ != http3_client_connection::outcome_type::pending || submission.id_ == 0) {
            throw http_client_error(http3_client_error_code(submission.outcome_),
                "HTTP/3 request could not be submitted");
        }
        try {
            connection->start_if_needed();
        } catch (...) {
            connection->request_stop();
            throw;
        }

        http3_client_connection::outcome_type outcome = http3_client_connection::outcome_type::pending;
        std::optional<http3_client_connection::rejected_request_type> rejected;
        {
            worker_cancellation_registration registration(cancellation_target_, response.state_->cancellation_id_);
            const auto cancellation_id = registration.id();
            quic_.register_cancellation(cancellation_id, connection, submission.id_);
            cancellation_guard cancellation{quic_, registration, cancellation_id};
            registration.arm(stop_token_value);

            co_await connection->wait(submission.id_);
            if (response.state_->http3_connection_ != connection) {
                // The public response may be abandoned and retired by its
                // waiter before this terminal notification is dispatched.
                quic_.generation_signal().notify();
                co_return;
            }
            const auto* result_value = connection->result(submission.id_);
            if (result_value == nullptr) {
                std::terminate();
            }
            outcome = result_value->outcome_;
            if (outcome == http3_client_connection::outcome_type::request_rejected) {
                if (!retried_rejected_request && !timeout.expired() && !stop_token_value.stop_requested()) {
                    auto handoff = connection->take_rejected_request(submission.id_);
                    if (handoff) {
                        rejected.emplace(std::move(*handoff));
                    }
                }
            } else if (response.state_->http3_connection_ == connection &&
                       !connection->release_response_request(submission.id_)) {
                std::terminate();
            }
        }
        quic_.generation_signal().notify();

        if (rejected) {
            wire.emplace(std::move(rejected->request_));
            absolute_deadline = rejected->deadline_;
            retried_rejected_request = true;
            connection_index = connection_count == 1
                                   ? selected_index
                                   : (selected_index + 1) % connection_count;
            if (timeout.expired()) {
                throw http_client_error(http_client_error::code_type::timeout,
                    "HTTP/3 request retry exceeded its deadline");
            }
            if (stop_token_value.stop_requested()) {
                throw http_client_error(http_client_error::code_type::cancelled,
                    "HTTP/3 request cancelled before retry");
            }
            continue;
        }
        if (outcome == http3_client_connection::outcome_type::request_rejected) {
            if (stop_token_value.stop_requested()) {
                throw http_client_error(http_client_error::code_type::cancelled,
                    "HTTP/3 request cancelled after peer rejection");
            }
            if (timeout.expired()) {
                throw http_client_error(http_client_error::code_type::timeout,
                    "HTTP/3 request deadline expired after peer rejection");
            }
            throw http_client_error(http_client_error::code_type::protocol_error,
                "HTTP/3 peer rejected the request as unprocessed");
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
    if (state_value.transport_ != http_client_response_transport::http2 || state_value.connection_index_ >= connections_.size()) {
        throw http_client_error(http_client_error::code_type::protocol_unavailable, "priority updates require HTTP/2 or HTTP/3");
    }
    auto& connection = connections_[state_value.connection_index_];
    if (!connection.http2_ || !connection.http2_runtime_->running_ || connection.http2_runtime_->failed_) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP/2 response connection is retired");
    }
    const auto status = connection.http2_->submit_priority_update(static_cast<std::uint32_t>(state_value.stream_id_),
        {.urgency_ = priority.urgency_, .incremental_ = priority.incremental_});
    if (status != http2_submit_status::accepted) {
        throw http_client_error(http_client_error::code_type::protocol_error, "HTTP/2 priority update rejected");
    }
    connection.http2_runtime_->write_signal_.notify();
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

task<http_client_tunnel_result> http_client_pool::open_tunnel(http_client_request_storage request, http_client_tunnel_config config, operation_options options) {
    if (!response_memory_) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP client pool is retired");
    }
    auto owned_request = std::move(request).into_resource(resource_);
    http_client_response response(*this);
    auto* state_value = response.state_;
    state_value->buffered_limit_ = config_.max_response_bytes_;
    state_value->tunnel_.emplace(state_value->memory_domain()->worker(), state_value->resource_, config);
    state_value->tunnel_->udp_ = owned_request.tunnel_protocol() == "connect-udp";
    if (state_value->tunnel_->udp_) {
        state_value->tunnel_->config_.datagrams_ = true;
    }
    owned_request.bind_tunnel(*state_value->tunnel_);
    background_tasks_.spawn(execute_into(std::move(owned_request), std::move(options), state_value));
    while (!state_value->head_ready_ && !state_value->failure_ && !state_value->error_code_) {
        co_await state_value->head_signal_.wait();
    }
    if (state_value->failure_) {
        std::rethrow_exception(state_value->failure_);
    }
    if (state_value->error_code_) {
        throw http_client_error(static_cast<http_client_error::code_type>(*state_value->error_code_), "CONNECT handshake failed");
    }
    if (state_value->tunnel_->accepted_) {
        co_return http_client_tunnel_result(http_client_tunnel(std::move(response)));
    }
    if (state_value->body_decode_required_) {
        if (state_value->http2_data_credit_) {
            release_response_data(*state_value);
        }
        state_value->notify_producer_space();
        while (!state_value->complete_) {
            co_await state_value->data_signal_.wait();
        }
        if (state_value->failure_) {
            std::rethrow_exception(state_value->failure_);
        }
        if (state_value->error_code_) {
            throw http_client_error(static_cast<http_client_error::code_type>(*state_value->error_code_), "CONNECT rejection body failed");
        }
    }
    co_return http_client_tunnel_result(std::move(response));
}

task<http_client_exchange> http_client_pool::open_request(http_client_request_storage request, http_client_upload_config upload, operation_options options) {
    if (!response_memory_) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP client pool is retired");
    }
    auto owned_request = std::move(request).into_resource(resource_);
    http_client_response response(*this);
    auto* state_value = response.state_;
    state_value->buffered_limit_ = config_.max_response_bytes_;
    state_value->upload_.emplace(state_value->memory_domain()->worker(), state_value->resource_, upload);
    state_value->upload_->content_released_ = upload.expectation_ == http_client_request_expectation::none;
    owned_request.bind_upload(*state_value->upload_);
    background_tasks_.spawn(execute_into(std::move(owned_request), std::move(options), state_value));
    co_return http_client_exchange(std::move(response));
}

task<http_client_response> http_client_pool::execute(
    http_client_request_storage request, operation_options options) {
    if (!response_memory_) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP client pool is retired");
    }
    // The request owns all data before transport work can outlive the caller.
    auto owned_request = std::move(request).into_resource(resource_);
    http_client_response response(*this);
    auto* state_value = response.state_;
    state_value->buffered_limit_ = config_.max_response_bytes_;
    background_tasks_.spawn(execute_into(std::move(owned_request), std::move(options), state_value));
    while (!state_value->head_ready_ && !state_value->failure_ && !state_value->error_code_) {
        co_await state_value->head_signal_.wait();
    }
    if (state_value->failure_) {
        std::rethrow_exception(state_value->failure_);
    }
    if (state_value->error_code_) {
        const auto code = static_cast<http_client_error::code_type>(*state_value->error_code_);
        throw http_client_error(code, "HTTP client request failed before the response head");
    }
    if (state_value->body_decode_required_) {
        if (state_value->http2_data_credit_) {
            release_response_data(*state_value);
        }
        state_value->notify_producer_space();
        while (!state_value->complete_) {
            co_await state_value->data_signal_.wait();
        }
        if (state_value->failure_) {
            std::rethrow_exception(state_value->failure_);
        }
        if (state_value->error_code_) {
            throw http_client_error(static_cast<http_client_error::code_type>(*state_value->error_code_),
                "HTTP client encoded response failed");
        }
    }
    co_return response;
}

task<void> http_client_pool::execute_into(
    http_client_request_storage request, operation_options options, http_client_response_state* state_value) {
    http_client_response keep_alive(state_value, true);
    try {
        if (state_value->abandoned_) {
            throw http_client_error(http_client_error::code_type::cancelled, "HTTP request abandoned before transport admission");
        }
        co_await execute_request_into(std::move(request), std::move(options), state_value);
    } catch (const http_client_error&) {
        state_value->failure_ = std::current_exception();
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

task<void> http_client_pool::execute_request_into(
    http_client_request_storage request, operation_options options, http_client_response_state* state_value) {
    http_client_response response(state_value, true);
    const ruvia::operation_timeout timeout(
        options.timeout_.has_value() ? options.timeout_ : config_.request_timeout_);
    const auto acquire_timeout = timeout.constrained_by(config_.acquire_timeout_);
    if (requests_buffered_ >= config_.max_buffered_requests_) {
        ++failed_requests_;
        throw http_client_error(
            http_client_error::code_type::queue_full, "http client request buffer is full");
    }
    ++requests_buffered_;
    std::size_t index = 0;
    try {
        index = co_await acquire(timeout, options.stop_token_);
    } catch (...) {
        --requests_buffered_;
        ++failed_requests_;
        throw;
    }
    --requests_buffered_;
    ++requests_in_flight_;
    lease_type lease_value(*this, index);
    if (config_.protocol_ == http_client_protocol::http3_only) {
        state_value->connection_index_ = index % quic_.size();
        try {
            co_await execute_http3(
                state_value->connection_index_, request, timeout, options.stop_token_, response);
            if (state_value->head_ready_ && !state_value->failure_ && !state_value->error_code_) {
                policy_.retain_response_cookies(request, response.headers());
            }
            --requests_in_flight_;
            if (state_value->failure_ || state_value->error_code_) {
                ++failed_requests_;
            } else {
                ++completed_requests_;
            }
            co_return;
        } catch (...) {
            --requests_in_flight_;
            ++failed_requests_;
            throw;
        }
    }

    auto& connection = lease_value.connection();
    state_value->connection_index_ = index % connections_.size();
    bool discard_connection = true;
    try {
        // A cancelled HTTP/1 producer still owns its SSL stream until its
        // serialized exchange unwinds. Join that exchange before reconnecting,
        // including when negotiated HTTP/2-capacity leases share the pool slot.
        if (connection.connected_ || connection.http2_runtime_->http1_operations_ == 0) {
            co_await ensure_connected(connection, timeout, acquire_timeout, options.stop_token_);
        }
        if (connection.protocol_ == wire_protocol_type::http2) {
            // This lease now shares a multiplexed session with background drivers
            // and possibly other requests. A request-local failure must not
            // discard that shared connection.
            discard_connection = false;
            co_await execute_http2(connection, request, timeout, options.stop_token_, response);
        } else {
            // A negotiated HTTP/1 connection can have several operations that
            // already hold outer HTTP/2-capacity slots. Waiting for this
            // connection's single exchange slot does not own its socket: a
            // timeout/cancellation here must not close the exchange currently
            // using it.
            discard_connection = false;
            auto& runtime = *connection.http2_runtime_;
            const bool must_buffer = runtime.http1_operations_ != 0;
            if (must_buffer && requests_buffered_ >= config_.max_buffered_requests_) {
                throw http_client_error(
                    http_client_error::code_type::queue_full, "HTTP client request buffer is full");
            }
            bool retry_as_http2 = false;
            {
                ++runtime.http1_operations_;
                if (must_buffer) {
                    --requests_in_flight_;
                    ++requests_buffered_;
                }
                struct h1_operation final {
                    http_client_pool& pool_;
                    http2_runtime_type& runtime_;
                    bool buffered_;
                    ~h1_operation() {
                        if (buffered_) {
                            --pool_.requests_buffered_;
                            ++pool_.requests_in_flight_;
                        }
                        if (runtime_.http1_operations_ == 0) {
                            std::terminate();
                        }
                        --runtime_.http1_operations_;
                    }
                } h1_operation_value{*this, runtime, must_buffer};
                auto h1_acquired = co_await connection.http2_runtime_->http1_scheduler_.acquire(
                    acquire_timeout.remaining(), options.stop_token_);
                if (h1_operation_value.buffered_) {
                    --requests_buffered_;
                    ++requests_in_flight_;
                    h1_operation_value.buffered_ = false;
                }
                switch (h1_acquired.status()) {
                    case ruvia::pool_waiter_result::status_type::acquired:
                        break;
                    case ruvia::pool_waiter_result::status_type::timed_out:
                        throw http_client_error(http_client_error::code_type::timeout,
                            "HTTP/1 connection acquire timed out");
                    case ruvia::pool_waiter_result::status_type::cancelled:
                        throw http_client_error(
                            http_client_error::code_type::cancelled, "HTTP/1 request cancelled");
                    case ruvia::pool_waiter_result::status_type::closed:
                        throw http_client_error(
                            http_client_error::code_type::closing, "HTTP client pool is closing");
                    default:
                        std::terminate();
                }
                const auto h1_slot = h1_acquired.index();
                struct h1_release final {
                    pool_lease_scheduler& scheduler_;
                    std::size_t slot_;
                    ~h1_release() {
                        (void)scheduler_.release(slot_);
                    }
                } h1_release_value{connection.http2_runtime_->http1_scheduler_, h1_slot};

                // A previous HTTP/1 exchange may have closed this connection while
                // this request waited for the serial exchange slot. Reconnect only
                // after acquiring that slot, so the connection cannot be replaced
                // underneath an active HTTP/1 exchange.
                discard_connection = true;
                try {
                    co_await ensure_connected(
                        connection, timeout, acquire_timeout, options.stop_token_);
                } catch (...) {
                    close(connection);
                    discard_connection = false;
                    throw;
                }
                if (connection.protocol_ == wire_protocol_type::http2) {
                    retry_as_http2 = true;
                } else {
                    connection.transport_.reset_abort();
                    ++connection.generation_;
                    if (connection.generation_ == 0) {
                        ++connection.generation_;
                    }
                    struct h1_cancellation_generation final {
                        connection_type& connection_;
                        ~h1_cancellation_generation() {
                            ++connection_.generation_;
                        }
                    } cancellation_generation{connection};
                    worker_cancellation_registration cancellation(cancellation_target_, connection.cancellation_id_);
                    state_value->cancellation_id_ = cancellation.id();
                    cancellation.arm(options.stop_token_);
                    connection.transport_.bind_response(state_value);
                    struct active_http1_response_guard final {
                        connection_type& connection_;
                        ~active_http1_response_guard() {
                            connection_.transport_.bind_response(nullptr);
                        }
                    } active_http1_response_guard_value{connection};
                    try {
                        co_await execute_http1(connection, request, timeout, response);
                    } catch (...) {
                        // Release the serial exchange slot only after the failed
                        // request has invalidated its connection. Pool handoff
                        // resumes the next waiter synchronously.
                        close(connection);
                        discard_connection = false;
                        throw;
                    }
                }
            }
            if (retry_as_http2) {
                discard_connection = false;
                co_await execute_http2(connection, request, timeout, options.stop_token_, response);
            }
        }
        policy_.retain_response_cookies(request, response.headers());
        --requests_in_flight_;
        ++completed_requests_;
        co_return;
    } catch (...) {
        --requests_in_flight_;
        ++failed_requests_;
        if (discard_connection) {
            lease_value.discard();
        }
        throw;
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
                cancel_http2_stream(
                    connections_[state_value.connection_index_], state_value.request_id_, client_abort_reason::cancelled);
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
            } else if (state_value.connection_index_ < connections_.size() && connections_[state_value.connection_index_].transport_.response() == &state_value) {
                close(connections_[state_value.connection_index_]);
            }
            break;
    }
    if (auto* output = state_value.output()) {
        output->stop();
    }
    state_value.space_signal_.notify();
}

void http_client_pool::release_response_data(http_client_response_state& state_value) noexcept {
    if (!state_value.http2_data_credit_) {
        return;
    }
    // The token returns credit to its original connection even if that session
    // has already retired. Its destructor also defers allocation failures.
    state_value.http2_data_credit_.reset();
    if (state_value.connection_index_ < connections_.size()) {
        connections_[state_value.connection_index_].http2_runtime_->write_signal_.notify();
    }
}

}  // namespace ruvia::detail
