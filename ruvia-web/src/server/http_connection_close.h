#pragma once

#include <cstdint>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia::detail {

// How a TCP connection session ended, which decides how its socket closes.
enum class http_connection_close : std::uint8_t {
    // Failure, timeout, worker stop, or a vanished peer: close immediately.
    abort,
    // The peer ended its input cleanly (FIN or TLS close_notify). TLS answers
    // with its own close_notify (RFC 8446 §6.1); no input remains to drain.
    peer_finished,
    // The server closes after a complete final response. RFC 9112 §9.6 staged
    // close: TLS close_notify, half-close, then drain unread input so a reset
    // cannot destroy the response the peer has not read yet.
    after_response,
};

// Worker-owned graceful close of a non-aborted session. The whole close is
// bounded by a fixed lingering deadline and the connection write timeout,
// drains at most a fixed byte budget, and registers with the worker scanner so
// worker stop closes the socket at once. The caller closes the socket after
// completion and must not start a graceful close once the worker is stopping.
[[nodiscard]] task<void> close_http_connection_gracefully(asio::ip::tcp::socket& socket,
    connection_scanner& scanner, const worker_handle& worker_value, http_connection_close mode);
[[nodiscard]] task<void> close_http_connection_gracefully(asio::ssl::stream<asio::ip::tcp::socket&>& stream,
    connection_scanner& scanner, const worker_handle& worker_value, http_connection_close mode);

}  // namespace ruvia::detail
