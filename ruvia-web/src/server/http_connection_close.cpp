#include "server/http_connection_close.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <system_error>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/ssl.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/async.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia::detail {
namespace {

// Total time a graceful close may take, including the close_notify flush.
constexpr auto graceful_close_time = std::chrono::seconds(2);
// Unread request content discarded during a staged close before giving up.
constexpr std::size_t lingering_close_bytes = std::size_t{4} * 1024 * 1024;

// Bounds one graceful close. The scanner entry keeps the socket reachable by
// worker stop and applies the write timeout; the deadline closes the socket
// so a pending flush or drain completes immediately. Destruction unregisters
// both before the caller closes the socket.
class graceful_close_bounds final {
public:
    graceful_close_bounds(asio::ip::tcp::socket& socket, connection_scanner& scanner,
        const worker_handle& worker_value)
        : guard_(scanner, entry_, socket) {
        entry_.set_phase(connection_scanner::phase_type::writing);
        armed_ = worker_value.schedule_timer(timer_, worker_timer_deadline_after(graceful_close_time),
                     [target = &socket](worker_timer_outcome outcome) {
                         if (outcome == worker_timer_outcome::expired) {
                             close_socket(*target);
                         }
                     }) == worker_timer_schedule_status::scheduled;
    }

    graceful_close_bounds(const graceful_close_bounds&) = delete;
    graceful_close_bounds& operator=(const graceful_close_bounds&) = delete;

    // False when the worker timer queue is already stopping; the caller then
    // closes immediately instead of running an unbounded close.
    [[nodiscard]] bool armed() const noexcept {
        return armed_;
    }

private:
    connection_scanner::entry_type entry_;
    connection_scanner::guard_type guard_;
    worker_timer_registration timer_;
    bool armed_{false};
};

// RFC 8446 §6.1: send close_notify once without waiting for the peer's.
// Recording the peer's alert as received lets SSL_shutdown complete as soon as
// this side's alert is flushed; anything the peer still sends is drained as
// raw bytes by the staged close.
task<bool> send_tls_close_notify(asio::ssl::stream<asio::ip::tcp::socket&>& stream) {
    SSL* const ssl = stream.native_handle();
    const int state = SSL_get_shutdown(ssl);
    if ((state & SSL_SENT_SHUTDOWN) != 0) {
        co_return true;
    }
    SSL_set_shutdown(ssl, state | SSL_RECEIVED_SHUTDOWN);
    const auto completion = co_await async_asio([&stream](auto handler) {
        stream.async_shutdown(std::move(handler));
    });
    co_return !completion.error_code();
}

// One frame owns the close bounds through TLS notification and input drain.
// The public entry points only select the transport; they do not await another
// close coroutine just to keep the same bounds alive.
task<void> close_transport_gracefully(asio::ip::tcp::socket& socket, connection_scanner& scanner,
    const worker_handle& worker_value, asio::ssl::stream<asio::ip::tcp::socket&>* tls_stream,
    http_connection_close mode) {
    const graceful_close_bounds bounds(socket, scanner, worker_value);
    if (!bounds.armed()) {
        co_return;
    }
    if (tls_stream != nullptr) {
        if (!co_await send_tls_close_notify(*tls_stream)) {
            co_return;
        }
        if (mode != http_connection_close::after_response) {
            co_return;
        }
    }

    // RFC 9112 §9.6: half-close, then discard input until the peer closes,
    // the byte budget is spent, or the bounds close the socket.
    std::error_code error;
    socket.shutdown(asio::ip::tcp::socket::shutdown_send, error);
    if (error) {
        co_return;
    }
    std::array<char, 4096> discard;
    std::size_t drained = 0;
    while (drained < lingering_close_bytes) {
        const auto completion = co_await async_asio<std::size_t>([&socket, &discard](auto handler) {
            socket.async_read_some(asio::buffer(discard.data(), discard.size()), std::move(handler));
        });
        if (completion.error_code()) {
            co_return;
        }
        drained += completion.result();
    }
}

}  // namespace

task<void> close_http_connection_gracefully(
    asio::ip::tcp::socket& socket, connection_scanner& scanner, const worker_handle& worker_value) {
    return close_transport_gracefully(
        socket, scanner, worker_value, nullptr, http_connection_close::after_response);
}

task<void> close_http_connection_gracefully(asio::ssl::stream<asio::ip::tcp::socket&>& stream,
    connection_scanner& scanner, const worker_handle& worker_value, http_connection_close mode) {
    return close_transport_gracefully(stream.next_layer(), scanner, worker_value, &stream, mode);
}

}  // namespace ruvia::detail
