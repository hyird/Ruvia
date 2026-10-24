#pragma once

#include <cstdint>
#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"

#include "server/connection_scanner.h"
#include "server/server_tls_stream.h"

namespace ruvia::detail {

// How a TCP connection session ended, which decides how its socket closes.
// A stopping worker closes every connection immediately, whatever its mode.
enum class http_connection_close : std::uint8_t {
    // Transport failure, a stalled transfer, or a vanished peer: close at once.
    abort,
    // No further peer input is expected: the peer ended its input (FIN or TLS
    // close_notify), or this side ended a wait for a request head (timeout or
    // drain). TLS sends close_notify first (RFC 9112 §9.8, RFC 9846 §6.1).
    notify,
    // This side ended the connection while peer input may still arrive: after
    // the final HTTP/1 response, or after HTTP/2 GOAWAY. Lingering close
    // (RFC 9112 §9.6): TLS close_notify, TCP send shutdown, then peer input is
    // read and discarded until its end, a deadline or a byte budget, so unread
    // input cannot make TCP reset the connection and destroy the last response.
    linger,
};

// Half-closes plain TCP after the final response. The session's close still
// lingers and closes the socket.
inline void shutdown_http_connection_send(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
}

// The lingering close of a plain connection, short of the final TCP close.
// The caller closes TCP after completion and must not start this operation
// once the worker is stopping; scanner close_all, a fixed deadline and the
// worker scanner bound it. The task borrows its arguments until completion.
[[nodiscard]] task<void> linger_http_connection(
    asio::ip::tcp::socket& socket, connection_scanner& scanner, const worker_handle& worker_value);

// Sends TLS close_notify for a non-aborted session without waiting for the
// peer's alert, then lingers like linger_http_connection() when `mode` is
// http_connection_close::linger. Same bounds and caller duties as above.
[[nodiscard]] task<void> close_http_connection_gracefully(server_tls_stream& stream,
    http_connection_close mode, connection_scanner& scanner, const worker_handle& worker_value);

}  // namespace ruvia::detail
