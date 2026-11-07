#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/socket_base.hpp>
#include <asio/write.hpp>

#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/websocket.h"

#include "tcp_port_reservation.h"

namespace lifecycle_test {

using namespace std::chrono_literals;

struct shared_state final {
    std::counting_semaphore<16> started_{0};
    std::atomic<unsigned> stop_hooks_{};
    std::atomic<bool> slow_entered_{};
    std::atomic<bool> release_slow_{};
    std::atomic<bool> slow_saw_stop_{};
    std::atomic<bool> stuck_entered_{};
    std::atomic<bool> stuck_cancelled_{};
    std::atomic<bool> upgrade_held_{};
    std::atomic<bool> upgrade_handler_ran_{};
    std::atomic<bool> socket_open_{};
    std::atomic<bool> tunnel_open_{};
};

shared_state& state() {
    static shared_state value;
    return value;
}

// Holds an upgrade inside its route chain until stop, polling on the worker
// so the worker keeps serving lifecycle controls meanwhile.
class held_upgrade final {
public:
    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next_value) {
        auto& state_value = state();
        state_value.upgrade_held_ = true;
        const auto stop = context.stop_token();
        while (!stop.stop_requested()) {
            (void)co_await ruvia::sleep_for(context.worker(), 1ms, stop);
        }
        co_await next_value();
    }
};

class lifecycle_controller final {
public:
    RUVIA_ROUTES_BEGIN(lifecycle_controller)
    RUVIA_GET("/fast", fast);
    RUVIA_GET("/slow", slow);
    RUVIA_GET("/stuck", stuck);
    RUVIA_GET_WS("/held-socket", held_socket, held_upgrade);
    RUVIA_GET_WS("/socket", open_socket);
    RUVIA_CONNECT("lifecycle-target.test:443", echo_tunnel);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> fast(ruvia::context& context) {
        co_return context.text(std::string_view("fast"));
    }

    // Finishes once the test releases it or the worker stops, polling on the
    // worker so the worker keeps serving lifecycle controls meanwhile.
    ruvia::task<ruvia::http_response> slow(ruvia::context& context) {
        auto& state_value = state();
        state_value.slow_entered_ = true;
        const auto stop = context.stop_token();
        while (!state_value.release_slow_.load() && !stop.stop_requested()) {
            (void)co_await ruvia::sleep_for(context.worker(), 1ms, stop);
        }
        state_value.slow_saw_stop_ = stop.stop_requested();
        co_return context.text(std::string_view("slow-done"));
    }

    // Ends only on its stop token.
    ruvia::task<ruvia::http_response> stuck(ruvia::context& context) {
        auto& state_value = state();
        state_value.stuck_entered_ = true;
        const auto result = co_await ruvia::sleep_for(context.worker(), 1h, context.stop_token());
        state_value.stuck_cancelled_ = result == ruvia::timer_sleep_result::stop_requested;
        co_return context.text(std::string_view("stuck-done"));
    }

    // Lives until the peer ends the session.
    ruvia::task<void> held_socket(ruvia::context& context) {
        state().upgrade_handler_ran_ = true;
        auto& websocket_value = context.websocket();
        try {
            while (co_await websocket_value.read()) {
            }
        } catch (...) {
        }
    }

    // Reads until the session ends.
    ruvia::task<void> open_socket(ruvia::context& context) {
        auto& websocket_value = context.websocket();
        state().socket_open_ = true;
        try {
            while (co_await websocket_value.read()) {
            }
        } catch (...) {
        }
    }

    // Echoes until the peer ends its send direction.
    ruvia::task<void> echo_tunnel(ruvia::context& context) {
        auto& tunnel_value = context.tunnel();
        state().tunnel_open_ = true;
        while (auto bytes = co_await tunnel_value.read()) {
            co_await tunnel_value.write(std::string_view(*bytes));
        }
        co_await tunnel_value.finish();
    }
};

// Hooks accumulate on the process-wide application, so every case shares one set.
void install_hooks() {
    static std::once_flag once;
    std::call_once(once, [] {
        ruvia::app()
            .on_start([] { state().started_.release(); })
            .on_stop([] { state().stop_hooks_.fetch_add(1); });
    });
}

class app_run final {
public:
    explicit app_run(ruvia::application& app)
        : app_(app),
          thread_([this] {
              try {
                  app_.run();
              } catch (...) {
                  failure_ = std::current_exception();
              }
          }) {}

    ~app_run() {
        if (thread_.joinable()) {
            app_.stop();
            thread_.join();
        }
    }

    app_run(const app_run&) = delete;
    app_run& operator=(const app_run&) = delete;

    void join() {
        thread_.join();
    }

    [[nodiscard]] std::exception_ptr failure() const noexcept {
        return failure_;
    }

private:
    ruvia::application& app_;
    std::exception_ptr failure_;
    std::thread thread_;
};

template <typename predicate_type>
[[nodiscard]] bool wait_for(predicate_type&& predicate, std::chrono::milliseconds limit = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// Appends received bytes until `done` holds or the peer ends the stream.
// Returns true when the peer ended it.
template <typename predicate_type>
bool read_until(asio::ip::tcp::socket& socket, std::string& received, predicate_type&& done,
    std::chrono::milliseconds limit = 5s) {
    socket.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::array<char, 1024> buffer;
    while (!done(received) && std::chrono::steady_clock::now() < deadline) {
        asio::error_code error;
        const auto size = socket.read_some(asio::buffer(buffer), error);
        received.append(buffer.data(), size);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if (error) {
            return true;
        }
    }
    return false;
}

bool read_to_end(asio::ip::tcp::socket& socket, std::string& received, std::chrono::milliseconds limit = 5s) {
    return read_until(socket, received, [](const std::string&) { return false; }, limit);
}

// The bytes after the response head, or empty while the head is incomplete.
[[nodiscard]] std::string_view after_head(std::string_view received) {
    const auto head_end = received.find("\r\n\r\n");
    return head_end == std::string_view::npos ? std::string_view{} : received.substr(head_end + 4);
}

void send(asio::ip::tcp::socket& socket, std::string_view bytes) {
    asio::write(socket, asio::buffer(bytes));
}

void send_websocket_upgrade(asio::ip::tcp::socket& socket, std::string_view path) {
    send(socket, "GET ");
    send(socket, path);
    send(socket,
        " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
}

}  // namespace lifecycle_test

using namespace lifecycle_test;

TEST(Ruvia, app_multi_worker_restarts_after_idle_connections_and_pending_accepts) {
    install_hooks();
    auto& app = ruvia::app();
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        app.server({.worker_count_ = 2, .connection_scan_interval_ = 5ms, .max_connections_per_worker_ = 64});
        app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
        app_run run(app);
        EXPECT_TRUE(state().started_.try_acquire_for(3s));
        EXPECT_EQ(app.workers().size(), std::size_t{2});
        std::vector<asio::ip::tcp::socket> idle;
        if (cycle != 0) {
            // Connecting without sending data must not hold up later accepts.
            for (unsigned i = 0; i < 16; ++i) {
                idle.emplace_back(io);
                idle.back().connect(endpoint);
            }
            for (unsigned i = 0; i < 24; ++i) {
                asio::ip::tcp::socket aborted(io);
                aborted.connect(endpoint);
                aborted.set_option(asio::socket_base::linger(true, 0));
                aborted.close();
            }
            std::vector<asio::ip::tcp::socket> requests;
            for (unsigned i = 0; i < 24; ++i) {
                requests.emplace_back(io);
                requests.back().connect(endpoint);
                send(requests.back(), "GET /fast HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
            }
            for (auto& socket : requests) {
                std::string response;
                EXPECT_TRUE(read_to_end(socket, response));
                EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
                EXPECT_TRUE(response.ends_with("fast"));
            }
        }
        // Cycle zero stops with no client; later cycles also retire connected
        // peers that never sent a request. Rebinding the port exercises reuse.
        app.stop();
        run.join();
        EXPECT_TRUE(run.failure() == nullptr);
        for (auto& socket : idle) {
            std::string response;
            EXPECT_TRUE(read_to_end(socket, response));
            EXPECT_TRUE(response.empty());
        }
        EXPECT_TRUE(app.workers().empty());
        EXPECT_EQ(app.http_stats().active_connections_, std::size_t{0});
        asio::ip::tcp::socket stopped(io);
        asio::error_code error;
        stopped.connect(endpoint, error);
        EXPECT_TRUE(error == asio::error::connection_refused);
    }
}

// A stop disconnects directly, with no drain phase: an idle keep-alive
// connection, a connection still sending its request head and one with a
// request in flight all close at once without a response, the in-flight
// handler observes its stop token, and the stop hooks run once.
TEST(Ruvia, app_stop_closes_every_connection_at_once) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.slow_entered_ = false;
    state_value.release_slow_ = false;
    state_value.slow_saw_stop_ = false;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    const auto stop_hooks = state_value.stop_hooks_.load();

    app_run run(app);
    EXPECT_TRUE(state_value.started_.try_acquire_for(3s));

    // An idle keep-alive connection, after one served request.
    asio::ip::tcp::socket idle(io);
    idle.connect(endpoint);
    send(idle, "GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
    std::string idle_response;
    EXPECT_TRUE(!read_until(idle, idle_response, [](const std::string& value) { return value.ends_with("fast"); }));
    EXPECT_TRUE(idle_response.starts_with("HTTP/1.1 200 "));

    asio::ip::tcp::socket partial(io);
    partial.connect(endpoint);
    send(partial, "GET /fa");

    asio::ip::tcp::socket busy(io);
    busy.connect(endpoint);
    send(busy, "GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(wait_for([&] { return state_value.slow_entered_.load(); }));
    EXPECT_TRUE(wait_for([&] { return app.http_stats().active_connections_ == 3; }));

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    for (auto* socket : {&idle, &partial, &busy}) {
        std::string rest;
        EXPECT_TRUE(read_to_end(*socket, rest, 3s));
        EXPECT_TRUE(rest.empty());
    }
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
    EXPECT_TRUE(std::chrono::steady_clock::now() - stop_started < 3s);
    EXPECT_TRUE(state_value.slow_saw_stop_.load());
    EXPECT_EQ(state_value.stop_hooks_.load(), stop_hooks + 1);
    EXPECT_EQ(app.http_stats().active_connections_, std::size_t{0});
}

// A request that never ends by itself does not hold the stop: its connection
// closes without a response and its handler observes the stop token.
TEST(Ruvia, app_stop_cancels_a_request_that_never_ends) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.stuck_entered_ = false;
    state_value.stuck_cancelled_ = false;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    EXPECT_TRUE(state_value.started_.try_acquire_for(3s));
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    send(socket, "GET /stuck HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(wait_for([&] { return state_value.stuck_entered_.load(); }));

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    std::string response;
    EXPECT_TRUE(read_to_end(socket, response, 3s));
    EXPECT_TRUE(response.empty());
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
    EXPECT_TRUE(std::chrono::steady_clock::now() - stop_started < 3s);
    EXPECT_TRUE(state_value.stuck_cancelled_.load());
}

// An upgrade still inside its route chain when the stop begins is never
// established: its connection closes without a 101 and the session handler
// never runs.
TEST(Ruvia, app_stop_closes_an_upgrade_still_in_its_route_chain) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.upgrade_held_ = false;
    state_value.upgrade_handler_ran_ = false;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    EXPECT_TRUE(state_value.started_.try_acquire_for(3s));
    asio::ip::tcp::socket upgrading(io);
    upgrading.connect(endpoint);
    send_websocket_upgrade(upgrading, "/held-socket");
    EXPECT_TRUE(wait_for([&] { return state_value.upgrade_held_.load(); }));

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    std::string received;
    EXPECT_TRUE(read_to_end(upgrading, received, 3s));
    EXPECT_TRUE(received.empty());
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
    EXPECT_TRUE(!state_value.upgrade_handler_ran_.load());
    EXPECT_TRUE(std::chrono::steady_clock::now() - stop_started < 3s);
}

// An established WebSocket session is closed at once, without a Close
// handshake: nothing follows the 101 response before the connection ends.
TEST(Ruvia, app_stop_closes_websockets_without_close_handshake) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.socket_open_ = false;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    EXPECT_TRUE(state_value.started_.try_acquire_for(3s));
    asio::ip::tcp::socket client(io);
    client.connect(endpoint);
    send_websocket_upgrade(client, "/socket");
    std::string received;
    EXPECT_TRUE(!read_until(client, received, [](const std::string& value) { return value.find("\r\n\r\n") != std::string::npos; }));
    EXPECT_TRUE(received.starts_with("HTTP/1.1 101 "));
    EXPECT_TRUE(wait_for([&] { return state_value.socket_open_.load(); }));

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    EXPECT_TRUE(read_to_end(client, received, 3s));
    EXPECT_TRUE(after_head(received).empty());
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
    EXPECT_TRUE(std::chrono::steady_clock::now() - stop_started < 3s);
}

// A tunnel has no in-band close signal and is not waited for: the stop
// closes it at once, while its peer still holds it open.
TEST(Ruvia, app_stop_closes_tunnels_at_once) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.tunnel_open_ = false;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    EXPECT_TRUE(state_value.started_.try_acquire_for(3s));
    asio::ip::tcp::socket tunnel(io);
    tunnel.connect(endpoint);
    send(tunnel, "CONNECT lifecycle-target.test:443 HTTP/1.1\r\nHost: lifecycle-target.test:443\r\n\r\n");
    std::string head;
    EXPECT_TRUE(!read_until(tunnel, head, [](const std::string& value) { return value.ends_with("\r\n\r\n"); }));
    EXPECT_TRUE(head.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(wait_for([&] { return state_value.tunnel_open_.load(); }));
    send(tunnel, "ping");
    std::string echoed;
    EXPECT_TRUE(!read_until(tunnel, echoed, [](const std::string& value) { return value.size() >= 4; }));
    EXPECT_EQ(echoed, std::string("ping"));

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    std::string rest;
    EXPECT_TRUE(read_to_end(tunnel, rest, 3s));
    EXPECT_TRUE(rest.empty());
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
    EXPECT_TRUE(std::chrono::steady_clock::now() - stop_started < 3s);
}

// At max_connections_per_worker_, a new connection takes the slot of the
// connection idle longest between requests, whose socket closes; only when no
// connection is idle is the new one refused.
TEST(Ruvia, app_connection_cap_evicts_the_oldest_idle_connection) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.stuck_entered_ = false;
    state_value.slow_entered_ = false;
    state_value.release_slow_ = false;
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms, .max_connections_per_worker_ = 2});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    EXPECT_TRUE(state_value.started_.try_acquire_for(3s));
    const auto served = [](asio::ip::tcp::socket& socket) {
        send(socket, "GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
        std::string response;
        const bool ended =
            read_until(socket, response, [](const std::string& value) { return value.ends_with("fast"); });
        return !ended && response.starts_with("HTTP/1.1 200 ");
    };
    asio::ip::tcp::socket oldest(io);
    oldest.connect(endpoint);
    EXPECT_TRUE(served(oldest));
    asio::ip::tcp::socket newer(io);
    newer.connect(endpoint);
    EXPECT_TRUE(served(newer));
    EXPECT_TRUE(wait_for([&] { return app.http_stats().active_connections_ == 2; }));

    asio::ip::tcp::socket admitted(io);
    admitted.connect(endpoint);
    EXPECT_TRUE(served(admitted));
    std::string evicted;
    EXPECT_TRUE(read_to_end(oldest, evicted));
    EXPECT_TRUE(evicted.empty());
    EXPECT_TRUE(served(newer));
    EXPECT_EQ(app.http_stats().connections_refused_, std::size_t{0});

    // With a request in flight on both, no connection is idle.
    send(newer, "GET /stuck HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(wait_for([&] { return state_value.stuck_entered_.load(); }));
    send(admitted, "GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(wait_for([&] { return state_value.slow_entered_.load(); }));
    asio::ip::tcp::socket refused(io);
    refused.connect(endpoint);
    std::string refused_bytes;
    EXPECT_TRUE(read_to_end(refused, refused_bytes));
    EXPECT_TRUE(refused_bytes.empty());
    EXPECT_EQ(app.http_stats().connections_refused_, std::size_t{1});

    app.stop();
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
}

// Connection timeouts never fire early because of the scanner's coarse clock,
// even when the scan interval exceeds the timeout, and they still fire.
TEST(Ruvia, app_connection_timeouts_honor_coarse_scan_clock) {
    install_hooks();
    auto& app = ruvia::app();
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    constexpr auto header_timeout = 200ms;
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 300ms, .request_header_timeout_ = header_timeout});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    EXPECT_TRUE(state().started_.try_acquire_for(3s));

    // A head trickling in every 50 ms stays active across several scans.
    asio::ip::tcp::socket active(io);
    active.connect(endpoint);
    send(active, "GET /fast HTTP/1.1\r\nHost: localhost\r\nX-Pad: ");
    for (int i = 0; i < 24; ++i) {
        std::this_thread::sleep_for(50ms);
        send(active, "a");
    }
    send(active, "\r\n\r\n");
    std::string response;
    EXPECT_TRUE(!read_until(active, response, [](const std::string& value) { return value.ends_with("fast"); }));
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));

    // A connection that sends nothing is closed, but not before its timeout.
    const auto connected = std::chrono::steady_clock::now();
    asio::ip::tcp::socket silent(io);
    silent.connect(endpoint);
    std::string silent_bytes;
    EXPECT_TRUE(read_to_end(silent, silent_bytes, 5s));
    EXPECT_TRUE(std::chrono::steady_clock::now() - connected >= header_timeout);
    EXPECT_TRUE(silent_bytes.empty());

    app.stop();
    run.join();
    EXPECT_TRUE(run.failure() == nullptr);
}

#ifdef RUVIA_ENABLE_REDIS
// A stop while a worker capability is still connecting cancels startup: run()
// returns normally, and no worker handle was published before readiness.
TEST(Ruvia, app_stop_during_capability_connect_returns_normally) {
    install_hooks();
    auto& app = ruvia::app();
    asio::io_context io;
    // Accepts the Redis connection and never answers its AUTH.
    asio::ip::tcp::acceptor silent_redis(io, {asio::ip::address_v4::loopback(), 0});
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    app.server({.worker_count_ = 1});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    app.redis({.config_ = {.port_ = silent_redis.local_endpoint().port(),
                   .password_ = "secret",
                   .pool_size_per_worker_ = 1,
                   .connect_timeout_ = std::nullopt}});

    std::optional<app_run> run(std::in_place, app);
    silent_redis.non_blocking(true);
    asio::ip::tcp::socket peer(io);
    EXPECT_TRUE(wait_for([&] {
        asio::error_code error;
        silent_redis.accept(peer, error);
        return !error;
    }));
    std::string auth;
    EXPECT_TRUE(!read_until(peer, auth, [](const std::string& value) { return !value.empty(); }));
    EXPECT_TRUE(!auth.empty());
    EXPECT_TRUE(app.workers().empty());
    EXPECT_TRUE(!app.worker_for(std::uint64_t{0}).valid());

    app.stop();
    run->join();
    EXPECT_TRUE(run->failure() == nullptr);
    run.reset();
    app.redis(nullptr);
}
#endif
