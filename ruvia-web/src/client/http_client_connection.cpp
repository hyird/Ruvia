#include "client/http_client_connection.h"

#include "client/http_client_pool.h"
#include "client/http_client_response_state.h"
#include "client/http_client_transport_error.h"

namespace ruvia::detail {
namespace {

// One transport read: what an idle keep-alive connection keeps for the next
// response. Larger growth (long heads, a busy tunnel) goes back to the worker.
constexpr std::size_t connection_read_buffer_retained_bytes = std::size_t{16} * 1024;

[[nodiscard]] client_tcp_endpoint http_client_endpoint(const http_client_config_storage& config) noexcept {
    return {.host_ = config.host_,
        .port_ = config.port_,
        .tls_ = config.scheme_ == http_scheme::https ? &config.tls_ : nullptr,
        .tcp_ = config.tcp_,
        .alpn_ = client_alpn::http11};
}

}  // namespace

http_client_connection::http_client_connection(http_client_pool& pool)
    : transport_(pool.io_context(), pool.tls_context(), pool.worker(), http_client_endpoint(pool.config()),
          pool.wire_counters()),
      read_buffer_(pool.resource()),
      write_buffer_(pool.resource()),
      pool_(pool) {}

http_client_connection::~http_client_connection() = default;

void http_client_connection::throw_if_aborted() const {
    if (const auto reason = transport_.abort_reason(); reason != client_abort_reason::none) {
        throw http_client_error(http_client_abort_error(reason), http_client_abort_message(reason));
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
    abort_http1_response();
    transport_.close();
    connected_ = false;
    read_buffer_.clear();
    write_buffer_.clear();
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
    close();
}

void http_client_connection::begin_lease() {
    if (retired_) {
        throw http_client_error(http_client_error::code_type::closing, "http client pool is closing");
    }
    transport_.reset_abort();
}

void http_client_connection::cancel(client_abort_reason reason) noexcept {
    transport_.abort(reason);
    abort_http1_response();
    transport_.close();
    connected_ = false;
}

task<void> http_client_connection::ensure_connected(const operation_timeout& operation_timeout_value) {
    if (connected_) {
        if (transport_.idle_open()) {
            co_return;
        }
        // The peer retired the idle connection; reconnect instead of writing
        // the request into a closed stream.
        close();
    }
    // A stop already requested when the lease armed its cancellation closed
    // the transport; it must not reconnect.
    throw_if_aborted();
    const auto timeout = operation_timeout_value.constrained_by(pool_.config().connect_timeout_);
    const auto connected = co_await transport_.connect(timeout);
    if (!connected.ok()) {
        throw_http_client_transport_error(connected, "upstream closed while connecting");
    }
    if (!transport_.alpn_allows_http11()) {
        throw http_client_error(http_client_error::code_type::protocol_unavailable,
            "upstream negotiated an unsupported HTTP protocol");
    }
    connected_ = true;
}

}  // namespace ruvia::detail
