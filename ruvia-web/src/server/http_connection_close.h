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
enum class http_connection_close : std::uint8_t {
    // Failure, timeout, worker stop, or a vanished peer: close immediately.
    abort,
    // The peer ended its input cleanly (FIN or TLS close_notify). TLS answers
    // with its own close_notify (RFC 9846 §6.1).
    peer_finished,
    // The final response is complete. Plain TCP half-closes its send side,
    // TLS sends close_notify, and the owner closes without draining peer input.
    after_response,
};

// Session ownership and its count lease may outlive the transport. Finish a
// completed plain response before caller-side logging or frame retirement;
// the session fallback is harmless when its writer already closed TCP.
inline void close_plain_http_connection(asio::ip::tcp::socket& socket, bool send_shutdown) noexcept {
    if (!socket.is_open()) {
        return;
    }
    std::error_code ignored;
    if (send_shutdown) {
        socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    }
    socket.close(ignored);
}

// Flush TLS close_notify for a non-aborted session without waiting for the
// peer's alert or draining input. A fixed deadline, write timeout and worker
// scanner bound the attempt. The caller closes TCP after completion and must
// not start this operation once the worker is stopping.
// The returned task borrows the stream, scanner and worker until completion.
[[nodiscard]] task<void> close_http_connection_gracefully(server_tls_stream& stream,
    connection_scanner& scanner, const worker_handle& worker_value);

}  // namespace ruvia::detail
