#include "client/http_client_connection.h"

#include "ruvia/core/worker_cancellation.h"

#include "client/http_client_pool.h"
#include "client/http_client_response_state.h"
#include "client/http_client_transport_error.h"

namespace ruvia::detail {
namespace {

constexpr std::size_t connection_read_buffer_initial_bytes = std::size_t{16} * 1024;

[[nodiscard]] client_tcp_endpoint http_client_endpoint(const http_client_config_storage& config) noexcept {
    return {.host_ = config.host_,
        .port_ = config.port_,
        .tls_ = config.scheme_ == http_scheme::https ? &config.tls_ : nullptr,
        .tcp_ = config.tcp_,
        .alpn_ = client_alpn::http11};
}

}  // namespace

http_client_connection::http_client_connection(http_client_pool& pool, std::size_t index)
    : transport_(pool.io_context(), pool.tls_context(), pool.worker(), http_client_endpoint(pool.config()),
          pool.wire_counters()),
      read_buffer_(pool.resource()),
      write_buffer_(pool.resource()),
      pool_(pool),
      index_(index) {
    read_buffer_.reserve(connection_read_buffer_initial_bytes);
}

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

void http_client_connection::shutdown() noexcept {
    transport_.abort(client_abort_reason::closing);
    abort_http1_response();
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
}

bool http_client_connection::cancel_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id_ != cancellation_id) {
        return false;
    }
    cancellation_id_ = 0;
    cancel(generation_, client_abort_reason::cancelled);
    return true;
}

task<void> http_client_connection::ensure_connected(
    const operation_timeout& operation_timeout_value, stop_token stop_token_value) {
    if (connected_) {
        if (transport_.idle_open()) {
            co_return;
        }
        // The peer retired the idle connection; reconnect instead of writing
        // the request into a closed stream.
        close();
    }
    transport_.reset_abort();
    generation_ = generation_ + 1 == 0 ? 1 : generation_ + 1;
    struct connect_generation final {
        http_client_connection& connection_;
        ~connect_generation() {
            ++connection_.generation_;
        }
    } generation{*this};
    worker_cancellation_registration cancellation(pool_.cancellation_target(), cancellation_id_);
    cancellation.arm(stop_token_value);
    throw_if_aborted();
    const auto timeout = operation_timeout_value.constrained_by(pool_.config().connect_timeout_);
    const auto connected = co_await transport_.connect(timeout);
    if (!connected.ok()) {
        throw_http_client_transport_error(connected, "upstream closed while connecting");
    }
    if (transport_.secure() && !transport_.alpn().empty() && transport_.alpn() != "http/1.1") {
        throw http_client_error(http_client_error::code_type::protocol_unavailable,
            "upstream negotiated an unsupported HTTP protocol");
    }
    connected_ = true;
}

}  // namespace ruvia::detail
