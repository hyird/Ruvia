#include "server/http_connection_close.h"

#include <chrono>
#include <utility>

#include <asio/ssl.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/async.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia::detail {
namespace {

// Total time allowed to flush TLS close_notify.
constexpr auto tls_close_time = std::chrono::seconds(2);

// Keep the TLS transport reachable by stop and bound a blocked close_notify
// write. The owner closes TCP after the notification attempt completes.
class tls_close_bounds final {
public:
    tls_close_bounds(asio::ip::tcp::socket& socket, connection_scanner& scanner,
        const worker_handle& worker_value)
        : guard_(scanner, entry_, socket) {
        entry_.set_phase(connection_scanner::phase_type::writing);
        armed_ = worker_value.schedule_timer(timer_, worker_timer_deadline_after(tls_close_time),
                     [target = &socket](worker_timer_outcome outcome) {
                         if (outcome == worker_timer_outcome::expired) {
                             close_socket(*target);
                         }
                     }) == worker_timer_schedule_status::scheduled;
    }

    tls_close_bounds(const tls_close_bounds&) = delete;
    tls_close_bounds& operator=(const tls_close_bounds&) = delete;

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

}  // namespace

task<void> close_http_connection_gracefully(asio::ssl::stream<asio::ip::tcp::socket&>& stream,
    connection_scanner& scanner, const worker_handle& worker_value) {
    const tls_close_bounds bounds(stream.next_layer(), scanner, worker_value);
    if (!bounds.armed()) {
        co_return;
    }
    // RFC 9846 §6.1: send our alert, without waiting for the peer's. Recording
    // its alert as received makes SSL_shutdown finish after our write; this
    // terminal session never reads or reuses the TLS stream afterward.
    SSL* const ssl = stream.native_handle();
    const int state = SSL_get_shutdown(ssl);
    if ((state & SSL_SENT_SHUTDOWN) != 0) {
        co_return;
    }
    SSL_set_shutdown(ssl, state | SSL_RECEIVED_SHUTDOWN);
    // The caller closes TCP after either a successful flush or a failed attempt,
    // as before; no input-drain operation follows the notification.
    static_cast<void>(co_await async_asio([&stream](auto handler) {
        stream.async_shutdown(std::move(handler));
    }));
}

}  // namespace ruvia::detail
