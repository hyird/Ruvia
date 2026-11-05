#include "client/http_client_connection.h"

#include <algorithm>
#include <exception>
#include <system_error>
#include <utility>

#include "ruvia/core/worker_cancellation.h"

#include "client/http_client_pool.h"
#include "client/http_client_response_state.h"
#include "client/http_client_transport_error.h"

namespace ruvia::detail {
namespace {

// One transport read: what an idle keep-alive connection keeps for the next
// response. Larger growth (long heads, a busy tunnel) goes back to the worker.
constexpr std::size_t connection_read_buffer_retained_bytes = std::size_t{16} * 1024;

[[nodiscard]] client_alpn select_client_alpn(http_client_protocol protocol) noexcept {
    switch (protocol) {
        case http_client_protocol::negotiate:
            return client_alpn::negotiate;
        case http_client_protocol::http1_only:
            return client_alpn::http11;
        case http_client_protocol::http2_only:
            return client_alpn::http2;
    }
    std::terminate();
}

[[nodiscard]] client_tcp_endpoint http_client_endpoint(const http_client_config_storage& config) noexcept {
    return {.host_ = config.host_,
        .port_ = config.port_,
        .tls_ = config.scheme_ == http_scheme::https ? &config.tls_ : nullptr,
        .tcp_ = config.tcp_,
        .alpn_ = select_client_alpn(config.protocol_)};
}

}  // namespace

http_client_connection::http_client_connection(http_client_pool& pool, std::size_t index)
    : transport_(pool.io_context(), pool.tls_context(), pool.worker(), http_client_endpoint(pool.config()),
          pool.wire_counters(), pool.resource()),
      read_buffer_(pool.resource()),
      write_buffer_(pool.resource()),
      http2_(*this, transport_, pool.tasks(), pool.worker(), pool.config().write_timeout_,
          pool.config().http2_ping_interval_, pool.resource()),
      connect_scheduler_(1, pool.worker(), pool.resource()),
      http1_scheduler_(1, pool.worker(), pool.resource()),
      pool_(pool),
      index_(index),
      http2_streams_(pool.resource()) {
}

http_client_connection::~http_client_connection() = default;

std::size_t http_client_connection::available_http2_request_streams() const noexcept {
    return protocol_ == protocol_type::http2
               ? http2_.available_request_streams(
                     pool_.config().max_concurrent_http2_streams_per_connection_)
               : 0;
}

void http_client_connection::throw_if_aborted() const {
    if (const auto reason = transport_.abort_reason(); reason != client_abort_reason::none) {
        throw_http_client_abort(reason);
    }
}

void http_client_connection::abort_http1_response() noexcept {
    if (http1_response_ == nullptr) {
        return;
    }
    if (auto* output = http1_response_->output()) {
        output->stop();
    }
    http1_response_->space_signal_.notify();
}

void http_client_connection::close() noexcept {
    end_idle();
    if (http1_response_ != nullptr) {
        if (auto* output = http1_response_->output()) {
            output->stop();
        }
    }
    if (http2_.running()) {
        http2_.fail({.status_ = client_transport_status::io_failed,
            .error_ = std::make_error_code(std::errc::operation_canceled)});
        return;
    }
    transport_.close();
    connected_ = false;
    protocol_ = protocol_type::unknown;
    if (http2_streams_.empty()) {
        // Registered streams may still borrow the retired connection object.
        http2_.reset();
    }
    read_buffer_.clear();
    write_buffer_.clear();
}

void http_client_connection::begin_idle() noexcept {
    const auto& idle_timeout = pool_.config().idle_timeout_;
    if (!idle_timeout || !connected_ || idle_timer_.registered()) {
        return;
    }
    try {
        (void)pool_.worker().schedule_timer(idle_timer_, worker_timer_deadline_after(*idle_timeout),
            [this](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    close_idle();
                }
            });
    } catch (...) {
        // An idle connection without its bound is not kept.
        close();
    }
}

void http_client_connection::end_idle() noexcept {
    idle_timer_.cancel();
}

void http_client_connection::close_idle() noexcept {
    // Every use of the connection ends the idle bound first; these hold only
    // for a connection nothing reused.
    if (connected_ && http1_response_ == nullptr && http2_streams_.empty()) {
        close();
    }
}

void http_client_connection::retain_idle_buffers() noexcept {
    // Unread bytes belong to the transport; only an empty buffer is trimmed.
    if (read_buffer_.empty()) {
        read_buffer_.clear_retaining_small(connection_read_buffer_retained_bytes);
    }
    write_buffer_.clear_retaining_small();
}

void http_client_connection::shutdown() noexcept {
    retired_ = true;
    transport_.abort(client_abort_reason::closing);
    abort_http1_response();
    (void)connect_scheduler_.close();
    (void)http1_scheduler_.close();
    close();
}

void http_client_connection::cancel(std::uint64_t generation, client_abort_reason reason) noexcept {
    if (generation_ != generation) {
        return;
    }
    transport_.abort(reason);
    abort_http1_response();
    transport_.close();
    connected_ = false;
    protocol_ = protocol_type::unknown;
    http2_.wake_writer();
    http2_.state_signal().notify();
}

bool http_client_connection::cancel_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id_ == cancellation_id) {
        cancellation_id_ = 0;
        cancel(generation_, client_abort_reason::cancelled);
        return true;
    }
    for (auto* waiter = state_waiters_; waiter; waiter = waiter->next_) {
        if (waiter->cancellation_id_ == cancellation_id) {
            http2_.state_signal().notify();
            return true;
        }
    }
    const auto stream = std::ranges::find_if(http2_streams_,
        [cancellation_id](const http2_stream* registered) { return registered->cancellation_id_ == cancellation_id; });
    if (stream == http2_streams_.end()) {
        return false;
    }
    (*stream)->cancellation_id_ = 0;
    cancel_http2_stream((*stream)->request_id_, client_abort_reason::cancelled);
    return true;
}

task<bool> http_client_connection::ensure_connected(const operation_timeout& operation_timeout_value,
    const operation_timeout& acquire_timeout, stop_token stop_token_value,
    http_client_response_state& response) {
    const auto acquired = co_await connect_scheduler_.acquire(acquire_timeout.remaining(), stop_token_value);
    if (!acquired.has_value()) {
        throw_http_client_wait_failure(acquired.status(), "HTTP client connect wait");
    }
    const auto connect_slot = acquired.value();
    struct connect_lease final {
        pool_lease_scheduler& scheduler_;
        std::size_t slot_;
        ~connect_lease() {
            scheduler_.release(slot_);
        }
    } connect_lease_value{connect_scheduler_, connect_slot};
    if (response.abandoned_) {
        throw http_client_error(http_client_error::code_type::cancelled, "HTTP response was abandoned");
    }
    end_idle();
    if (retired_) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP client connection is retired");
    }
    if (connected_) {
        if (protocol_ == protocol_type::http2) {
            co_return true;
        }
        const auto idle_status = transport_.idle_status();
        if (idle_status == client_idle_transport_status::reusable) {
            co_return true;
        }
        if (idle_status == client_idle_transport_status::pending_data) {
            pool_.record_idle_connection_with_pending_data();
        }
        close();
    }
    if (stop_token_value.stop_requested()) {
        throw http_client_error(http_client_error::code_type::cancelled, "HTTP client connect cancelled");
    }
    transport_.reset_abort();
    generation_ = generation_ + 1 == 0 ? 1 : generation_ + 1;
    struct connect_generation final {
        http_client_connection& connection_;
        ~connect_generation() {
            ++connection_.generation_;
        }
    } connect_generation_value{*this};
    worker_cancellation_registration cancellation(pool_.cancellation_target(), cancellation_id_);
    response.cancellation_id_ = cancellation.id();
    struct response_connect_binding final {
        http_client_response_state& response_;
        ~response_connect_binding() {
            response_.cancellation_id_ = 0;
        }
    } binding{response};
    cancellation.arm(stop_token_value);
    // A previous HTTP/2 generation may still be unwinding its drivers and
    // streams; they borrow the transport that connect() replaces.
    while (http2_.running() || !http2_streams_.empty()) {
        throw_if_aborted();
        co_await http2_.state_signal().wait();
    }
    throw_if_aborted();
    http2_.reset();
    const auto& config = pool_.config();
    const auto timeout = operation_timeout_value.constrained_by(config.connect_timeout_);
    const auto connected = co_await transport_.connect(timeout);
    if (!connected.ok()) {
        close();
        throw_http_client_transport_error(connected, "upstream closed while connecting");
    }
    if (transport_.secure()) {
        const auto alpn = transport_.alpn();
        if (config.protocol_ == http_client_protocol::http2_only && alpn != "h2") {
            close();
            throw http_client_error(
                http_client_error::code_type::protocol_unavailable, "upstream did not negotiate HTTP/2");
        }
        protocol_ = alpn == "h2" ? protocol_type::http2 : protocol_type::http1;
    } else {
        protocol_ = config.protocol_ == http_client_protocol::http2_only ? protocol_type::http2 : protocol_type::http1;
    }
    connected_ = true;
    if (protocol_ != protocol_type::http2) {
        co_return false;
    }
    const auto started = co_await http2_.start({.enable_push_ = config.push_.enabled_,
                                                   .receive_origin_advertisements_ = config.advertisements_.receive_origins_},
        timeout);
    switch (started.status_) {
        case http2_client_session_start_status::started:
            co_return false;
        case http2_client_session_start_status::transport_failed:
            close();
            throw_http_client_transport_error(started.transport_, "upstream closed during HTTP/2 preface");
        case http2_client_session_start_status::protocol_failed:
            close();
            throw http_client_error(http_client_error::code_type::protocol_error, "invalid HTTP/2 connection preface");
    }
    std::terminate();
}

}  // namespace ruvia::detail
