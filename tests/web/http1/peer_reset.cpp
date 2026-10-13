#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <semaphore>
#include <string_view>
#include <system_error>
#include <thread>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/streaming.h"

#include "test_harness.h"

// A peer that resets its connection while a buffered handler is still running
// makes that handler's own transport write -- an interim head, or the 100
// Continue its body read sends -- fail. That is the connection ending, not the
// handler failing: the error handler does not see it, nothing is counted as a
// connection failure, and no 500 is attempted. A connection this server closes
// itself (here: idle_timeout during a stream handler's pause) ends just as
// normally for the handler's next write. This executable owns its application
// because the error handler is registered application-wide.

namespace peer_reset_test {

using namespace std::chrono_literals;

// Far above the reset cases' handler waits, so it never closes those first.
constexpr auto idle_timeout = 1s;

std::atomic<bool> peer_closed{false};
std::atomic<std::size_t> error_handler_calls{0};
std::atomic<std::size_t> handlers_finished{0};

ruvia::task<void> wait_for_peer_close(ruvia::context& c) {
    while (!peer_closed.load()) {
        (void)co_await ruvia::sleep_for(c.worker(), 10ms);
    }
    // The reset has reached this side by now; the next transport write
    // observes it.
    (void)co_await ruvia::sleep_for(c.worker(), 50ms);
}

class peer_reset_routes final {
    RUVIA_ROUTES_BEGIN(peer_reset_routes)
    RUVIA_GET("/peer-reset/inform", inform);
    RUVIA_POST("/peer-reset/body", body);
    RUVIA_GET_STREAM("/peer-reset/idle-stream", idle_stream);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> inform(ruvia::context& c) {
        co_await wait_for_peer_close(c);
        for (int attempt = 0; attempt < 4; ++attempt) {
            co_await c.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
            (void)co_await ruvia::sleep_for(c.worker(), 20ms);
        }
        handlers_finished.fetch_add(1);
        co_return c.text("informed");
    }

    ruvia::task<ruvia::http_response> body(ruvia::context& c) {
        co_await wait_for_peer_close(c);
        // The client expected 100 Continue and sent no body: the lazy read
        // writes the interim response first.
        (void)co_await c.req().text();
        handlers_finished.fetch_add(1);
        co_return c.text("read");
    }

    // Pauses past idle_timeout, so the server closes the connection before the
    // second write starts.
    ruvia::task<void> idle_stream(ruvia::context& c) {
        co_await c.stream_text().write("first");
        (void)co_await ruvia::sleep_for(c.worker(), idle_timeout + 500ms);
        co_await c.stream_text().write("second");
        handlers_finished.fetch_add(1);
    }
};

ruvia::task<ruvia::http_response> count_error(ruvia::context& c, ruvia::http_error_info error) {
    error_handler_calls.fetch_add(1);
    c.status(error.status());
    co_return c.text("error");
}

// Sends `request`, then closes with a zero linger so the close is a reset.
void send_and_reset(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view request) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    asio::write(socket, asio::buffer(request));
    // Let the server read the request before the reset follows it.
    std::this_thread::sleep_for(100ms);
    socket.set_option(asio::socket_base::linger(true, 0));
    std::error_code ignored;
    socket.close(ignored);
}

void wait_until_idle(ruvia::application& app) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (app.http_stats().active_connections_ != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
}

// Reads until the server closes the connection.
void send_and_read_to_close(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view request) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    asio::write(socket, asio::buffer(request));
    std::array<char, 1024> buffer{};
    std::error_code ec;
    while (!ec) {
        (void)socket.read_some(asio::buffer(buffer), ec);
    }
}

}  // namespace peer_reset_test

using namespace peer_reset_test;

RUVIA_TEST(a_peer_reset_during_a_handlers_transport_write_is_a_normal_connection_end) {
    asio::io_context io;
    asio::ip::tcp::acceptor reservation(io, {asio::ip::address_v4::loopback(), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();

    auto& app = ruvia::app();
    std::binary_semaphore started{0};
    app.server({.worker_count_ = 1, .idle_timeout_ = idle_timeout, .connection_scan_interval_ = 5ms})
        .listen({.address_ = "127.0.0.1", .http_ = endpoint.port()})
        .on_error(&count_error)
        .on_start([&started] { started.release(); });
    std::exception_ptr failure;
    std::thread runner([&app, &failure] {
        try {
            app.run();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    const bool ready = started.try_acquire_for(10s);
    RUVIA_CHECK(ready);
    if (ready) {
        send_and_reset(io, endpoint, "GET /peer-reset/inform HTTP/1.1\r\nHost: localhost\r\n\r\n");
        peer_closed.store(true);
        wait_until_idle(app);
        RUVIA_CHECK_EQ(app.http_stats().active_connections_, std::size_t{0});

        peer_closed.store(false);
        send_and_reset(io, endpoint,
            "POST /peer-reset/body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n");
        peer_closed.store(true);
        wait_until_idle(app);
        RUVIA_CHECK_EQ(app.http_stats().active_connections_, std::size_t{0});

        send_and_read_to_close(io, endpoint, "GET /peer-reset/idle-stream HTTP/1.1\r\nHost: localhost\r\n\r\n");
        wait_until_idle(app);
        RUVIA_CHECK_EQ(app.http_stats().active_connections_, std::size_t{0});

        RUVIA_CHECK_EQ(error_handler_calls.load(), std::size_t{0});
        RUVIA_CHECK_EQ(app.http_stats().connection_failures_, std::size_t{0});
        // The failed write ended each handler; none ran to its end.
        RUVIA_CHECK_EQ(handlers_finished.load(), std::size_t{0});
    }
    app.stop();
    runner.join();
    RUVIA_CHECK(failure == nullptr);
}
