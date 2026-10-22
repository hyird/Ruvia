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
#include "ruvia/web/next.h"
#include "ruvia/web/websocket.h"

#include "test_harness.h"

namespace lifecycle_test {

using namespace std::chrono_literals;

struct shared_state final {
    std::counting_semaphore<16> started_{0};
    std::atomic<unsigned> stop_hooks_{};
    std::atomic<bool> slow_entered_{};
    std::atomic<bool> release_slow_{};
    std::atomic<bool> slow_returned_{};
    std::atomic<bool> slow_saw_stop_{};
    std::atomic<bool> stop_hook_saw_slow_return_{};
    std::atomic<bool> stuck_entered_{};
    std::atomic<bool> stuck_cancelled_{};
    std::atomic<bool> upgrade_held_{};
    std::atomic<bool> release_upgrade_{};
    std::atomic<bool> upgrade_handler_ran_{};
    std::atomic<bool> socket_open_{};
    std::atomic<bool> socket_read_ended_{};
    std::atomic<bool> tunnel_open_{};
};

shared_state& state() {
    static shared_state value;
    return value;
}

// Holds an upgrade inside its route chain until the test releases it, polling
// on the worker so the worker keeps serving lifecycle controls meanwhile.
class held_upgrade final {
public:
    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next_value) {
        auto& state_value = state();
        state_value.upgrade_held_ = true;
        const auto stop = context.stop_token();
        while (!state_value.release_upgrade_.load() && !stop.stop_requested()) {
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
    const auto short_close = ruvia::websocket_route_config{.lifecycle_ = {.close_handshake_timeout_ = 200ms}};
    const auto unbounded_close =
        ruvia::websocket_route_config{.lifecycle_ = {.close_handshake_timeout_ = std::nullopt}};
    RUVIA_GET_WS("/socket", open_socket);
    RUVIA_GET_WS("/short-close-socket", open_socket, short_close);
    RUVIA_GET_WS("/unbounded-close-socket", open_socket, unbounded_close);
    RUVIA_CONNECT("lifecycle-target.test:443", echo_tunnel);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> fast(ruvia::context& context) {
        co_return context.text(std::string_view("fast"));
    }

    // Finishes only once the test releases it, polling on the worker so the
    // worker keeps serving lifecycle controls meanwhile.
    ruvia::task<ruvia::http_response> slow(ruvia::context& context) {
        auto& state_value = state();
        state_value.slow_entered_ = true;
        const auto stop = context.stop_token();
        while (!state_value.release_slow_.load() && !stop.stop_requested()) {
            (void)co_await ruvia::sleep_for(context.worker(), 1ms, stop);
        }
        state_value.slow_saw_stop_ = stop.stop_requested();
        state_value.slow_returned_ = true;
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

    // Reads until the session ends, recording whether its reads ended cleanly.
    ruvia::task<void> open_socket(ruvia::context& context) {
        auto& state_value = state();
        auto& websocket_value = context.websocket();
        state_value.socket_open_ = true;
        try {
            while (co_await websocket_value.read()) {
            }
            state_value.socket_read_ended_ = true;
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
            .on_stop([] {
                auto& state_value = state();
                state_value.stop_hook_saw_slow_return_ = state_value.slow_returned_.load();
                state_value.stop_hooks_.fetch_add(1);
            });
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

[[nodiscard]] asio::ip::tcp::endpoint reserve_endpoint(asio::io_context& io) {
    asio::ip::tcp::acceptor reservation(io, {asio::ip::address_v4::loopback(), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();
    return endpoint;
}

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

[[nodiscard]] bool has_header(std::string_view response, std::string_view line) {
    const auto head_end = response.find("\r\n\r\n");
    return head_end != std::string_view::npos && response.substr(0, head_end).find(line) != std::string_view::npos;
}

void send(asio::ip::tcp::socket& socket, std::string_view bytes) {
    asio::write(socket, asio::buffer(bytes.data(), bytes.size()));
}

void send_websocket_upgrade(asio::ip::tcp::socket& socket, std::string_view path) {
    send(socket, "GET ");
    send(socket, path);
    send(socket,
        " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
}

// The server's unmasked Close frame carrying 1001 Going Away.
constexpr std::string_view going_away_close("\x88\x02\x03\xE9", 4);
// The client's answering Close: masked, with an all-zero masking key.
constexpr std::string_view client_close_reply("\x88\x82\x00\x00\x00\x00\x03\xE9", 8);

// The bytes after the response head, or empty while the head is incomplete.
[[nodiscard]] std::string_view after_head(std::string_view received) {
    const auto head_end = received.find("\r\n\r\n");
    return head_end == std::string_view::npos ? std::string_view{} : received.substr(head_end + 4);
}

// Reads the 101 response and then the drain's Close frame.
[[nodiscard]] bool read_going_away(asio::ip::tcp::socket& socket, std::string& received) {
    (void)read_until(socket, received,
        [](const std::string& value) { return after_head(value).size() >= going_away_close.size(); });
    return received.starts_with("HTTP/1.1 101 ") && after_head(received) == going_away_close;
}

void reset_socket_state() {
    auto& state_value = state();
    state_value.socket_open_ = false;
    state_value.socket_read_ended_ = false;
}

}  // namespace lifecycle_test

using namespace lifecycle_test;

RUVIA_TEST(app_multi_worker_restarts_after_idle_connections_and_pending_accepts) {
    install_hooks();
    auto& app = ruvia::app();
    asio::io_context io;
    const auto endpoint = reserve_endpoint(io);
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        app.server({.worker_count_ = 2, .drain_timeout_ = 0ms, .connection_scan_interval_ = 5ms, .max_connections_per_worker_ = 64});
        app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
        app_run run(app);
        RUVIA_CHECK(state().started_.try_acquire_for(3s));
        RUVIA_CHECK_EQ(app.workers().size(), std::size_t{2});
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
                RUVIA_CHECK(read_to_end(socket, response));
                RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));
                RUVIA_CHECK(response.ends_with("fast"));
            }
        }
        // Cycle zero stops with no client; later cycles also retire connected
        // peers that never sent a request. Rebinding the port exercises reuse.
        app.stop();
        run.join();
        RUVIA_CHECK(run.failure() == nullptr);
        for (auto& socket : idle) {
            std::string response;
            RUVIA_CHECK(read_to_end(socket, response));
            RUVIA_CHECK(response.empty());
        }
        RUVIA_CHECK(app.workers().empty());
        RUVIA_CHECK_EQ(app.http_stats().active_connections_, std::size_t{0});
        asio::ip::tcp::socket stopped(io);
        asio::error_code error;
        stopped.connect(endpoint, error);
        RUVIA_CHECK(error == asio::error::connection_refused);
    }
}

// A stop while a request is in flight closes admission and idle connections at
// once, lets the request finish with Connection: close, and only then retires
// the workers and runs the stop hooks.
static void check_stop_drains_in_flight_request(
    ruvia::testing::test_context& ruvia_ctx, std::chrono::milliseconds drain_timeout) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.slow_entered_ = false;
    state_value.release_slow_ = false;
    state_value.slow_returned_ = false;
    state_value.slow_saw_stop_ = false;
    state_value.stop_hook_saw_slow_return_ = false;
    asio::io_context io;
    const auto endpoint = reserve_endpoint(io);
    app.server({.worker_count_ = 1, .drain_timeout_ = drain_timeout, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    const auto stop_hooks = state_value.stop_hooks_.load();

    app_run run(app);
    RUVIA_CHECK(state_value.started_.try_acquire_for(3s));

    // An idle keep-alive connection, after one served request.
    asio::ip::tcp::socket idle(io);
    idle.connect(endpoint);
    send(idle, "GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
    std::string idle_response;
    RUVIA_CHECK(!read_until(idle, idle_response, [](const std::string& value) { return value.ends_with("fast"); }));
    RUVIA_CHECK(idle_response.starts_with("HTTP/1.1 200 "));

    asio::ip::tcp::socket busy(io);
    busy.connect(endpoint);
    send(busy, "GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n");
    RUVIA_CHECK(wait_for([&] { return state_value.slow_entered_.load(); }));

    app.stop();
    // The drain closes the idle connection without waiting for the busy one.
    std::string idle_rest;
    RUVIA_CHECK(read_to_end(idle, idle_rest, 3s));
    RUVIA_CHECK(idle_rest.empty());
    RUVIA_CHECK(!state_value.slow_returned_.load());
    RUVIA_CHECK_EQ(state_value.stop_hooks_.load(), stop_hooks);

    state_value.release_slow_ = true;
    std::string busy_response;
    RUVIA_CHECK(read_to_end(busy, busy_response));
    RUVIA_CHECK(busy_response.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(has_header(busy_response, "Connection: close"));
    RUVIA_CHECK(busy_response.ends_with("slow-done"));

    run.join();
    RUVIA_CHECK(run.failure() == nullptr);
    RUVIA_CHECK(!state_value.slow_saw_stop_.load());
    RUVIA_CHECK_EQ(state_value.stop_hooks_.load(), stop_hooks + 1);
    RUVIA_CHECK(state_value.stop_hook_saw_slow_return_.load());
}

RUVIA_TEST(app_stop_drains_in_flight_requests) {
    check_stop_drains_in_flight_request(ruvia_ctx, 10s);
    // A large nonsaturated deadline must also allow the worker to notify completion.
    check_stop_drains_in_flight_request(ruvia_ctx, std::chrono::hours(24 * 365 * 250));
    // The largest configurable drain window saturates instead of overflowing
    // into a deadline that has already passed.
    check_stop_drains_in_flight_request(ruvia_ctx, std::chrono::milliseconds::max());
}

// A request still running at the drain deadline is cut: its connection closes
// without a response and its handler observes the stop token.
RUVIA_TEST(app_stop_forces_close_at_drain_deadline) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.stuck_entered_ = false;
    state_value.stuck_cancelled_ = false;
    asio::io_context io;
    const auto endpoint = reserve_endpoint(io);
    constexpr auto drain_timeout = 200ms;
    app.server({.worker_count_ = 1, .drain_timeout_ = drain_timeout, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    RUVIA_CHECK(state_value.started_.try_acquire_for(3s));
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    send(socket, "GET /stuck HTTP/1.1\r\nHost: localhost\r\n\r\n");
    RUVIA_CHECK(wait_for([&] { return state_value.stuck_entered_.load(); }));

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    std::string response;
    RUVIA_CHECK(read_to_end(socket, response));
    RUVIA_CHECK(response.empty());
    run.join();
    const auto elapsed = std::chrono::steady_clock::now() - stop_started;
    RUVIA_CHECK(run.failure() == nullptr);
    RUVIA_CHECK(elapsed >= drain_timeout);
    RUVIA_CHECK(state_value.stuck_cancelled_.load());
}

// An upgrade still inside its route chain when the drain starts is not admitted
// once it is established: the session handler never runs, and the connection
// answers the handshake with a 1001 Going Away Close, so it cannot hold the
// drain open.
RUVIA_TEST(app_drain_refuses_an_upgrade_established_after_the_drain_began) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.upgrade_held_ = false;
    state_value.release_upgrade_ = false;
    state_value.upgrade_handler_ran_ = false;
    asio::io_context io;
    const auto endpoint = reserve_endpoint(io);
    app.server({.worker_count_ = 1, .drain_timeout_ = 30s, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    RUVIA_CHECK(state_value.started_.try_acquire_for(3s));

    // An idle keep-alive connection whose close shows that the drain began.
    asio::ip::tcp::socket idle(io);
    idle.connect(endpoint);
    send(idle, "GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
    std::string idle_response;
    RUVIA_CHECK(!read_until(idle, idle_response, [](const std::string& value) { return value.ends_with("fast"); }));

    asio::ip::tcp::socket upgrading(io);
    upgrading.connect(endpoint);
    send_websocket_upgrade(upgrading, "/held-socket");
    RUVIA_CHECK(wait_for([&] { return state_value.upgrade_held_.load(); }));

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    std::string idle_rest;
    RUVIA_CHECK(read_to_end(idle, idle_rest, 3s));

    state_value.release_upgrade_ = true;
    std::string upgrade_response;
    RUVIA_CHECK(read_going_away(upgrading, upgrade_response));
    send(upgrading, client_close_reply);
    std::string upgrade_rest;
    RUVIA_CHECK(read_to_end(upgrading, upgrade_rest));
    RUVIA_CHECK(upgrade_rest.empty());
    run.join();
    RUVIA_CHECK(run.failure() == nullptr);
    RUVIA_CHECK(!state_value.upgrade_handler_ran_.load());
    RUVIA_CHECK(std::chrono::steady_clock::now() - stop_started < 10s);
}

// A drain asks every established WebSocket session to go away: the server
// starts the close handshake with 1001, the session's reads end cleanly once
// the peer answers, and the server then closes the connection without waiting
// for the drain deadline.
RUVIA_TEST(app_drain_closes_websockets_with_going_away) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    reset_socket_state();
    asio::io_context io;
    const auto endpoint = reserve_endpoint(io);
    app.server({.worker_count_ = 1, .drain_timeout_ = 30s, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    RUVIA_CHECK(state_value.started_.try_acquire_for(3s));
    asio::ip::tcp::socket client(io);
    client.connect(endpoint);
    send_websocket_upgrade(client, "/socket");
    RUVIA_CHECK(wait_for([&] { return state_value.socket_open_.load(); }));

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    std::string received;
    RUVIA_CHECK(read_going_away(client, received));
    send(client, client_close_reply);
    std::string rest;
    RUVIA_CHECK(read_to_end(client, rest));
    RUVIA_CHECK(rest.empty());
    run.join();
    RUVIA_CHECK(run.failure() == nullptr);
    RUVIA_CHECK(state_value.socket_read_ended_.load());
    RUVIA_CHECK(std::chrono::steady_clock::now() - stop_started < 10s);
}

// A peer that never answers the drain's Close is cut at the earlier of the
// route's close-handshake timeout and the drain deadline.
static void check_unanswered_going_away(ruvia::testing::test_context& ruvia_ctx, std::string_view path,
    std::chrono::milliseconds drain_timeout, std::chrono::milliseconds expected_bound) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    reset_socket_state();
    asio::io_context io;
    const auto endpoint = reserve_endpoint(io);
    app.server({.worker_count_ = 1, .drain_timeout_ = drain_timeout, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    RUVIA_CHECK(state_value.started_.try_acquire_for(3s));
    asio::ip::tcp::socket client(io);
    client.connect(endpoint);
    send_websocket_upgrade(client, path);
    RUVIA_CHECK(wait_for([&] { return state_value.socket_open_.load(); }));

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    std::string received;
    RUVIA_CHECK(read_going_away(client, received));
    std::string rest;
    RUVIA_CHECK(read_to_end(client, rest));
    RUVIA_CHECK(rest.empty());
    run.join();
    const auto elapsed = std::chrono::steady_clock::now() - stop_started;
    RUVIA_CHECK(run.failure() == nullptr);
    RUVIA_CHECK(elapsed >= expected_bound);
    RUVIA_CHECK(elapsed < expected_bound + 5s);
}

RUVIA_TEST(app_drain_bounds_an_unanswered_going_away) {
    // The route's close-handshake timeout ends it long before the drain deadline.
    check_unanswered_going_away(ruvia_ctx, "/short-close-socket", 30s, 200ms);
    // Without a close-handshake timeout, the drain deadline ends it.
    check_unanswered_going_away(ruvia_ctx, "/unbounded-close-socket", 300ms, 300ms);
}

// A connection still receiving a request head has no admitted request, so the
// drain closes it at once instead of holding the stop until the deadline.
RUVIA_TEST(app_drain_closes_a_partial_request_head) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    asio::io_context io;
    const auto endpoint = reserve_endpoint(io);
    app.server({.worker_count_ = 1, .drain_timeout_ = 30s, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    RUVIA_CHECK(state_value.started_.try_acquire_for(3s));
    // A connection's first request head, and a partial head pipelined behind a
    // served request on a kept-alive connection.
    asio::ip::tcp::socket first(io);
    first.connect(endpoint);
    send(first, "GET /fa");
    asio::ip::tcp::socket pipelined(io);
    pipelined.connect(endpoint);
    send(pipelined, "GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\nGET /fa");
    std::string pipelined_response;
    RUVIA_CHECK(!read_until(
        pipelined, pipelined_response, [](const std::string& value) { return value.ends_with("fast"); }));
    RUVIA_CHECK(wait_for([&] { return app.http_stats().active_connections_ == 2; }));

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    std::string first_rest;
    RUVIA_CHECK(read_to_end(first, first_rest, 3s));
    RUVIA_CHECK(first_rest.empty());
    std::string pipelined_rest;
    RUVIA_CHECK(read_to_end(pipelined, pipelined_rest, 3s));
    RUVIA_CHECK(pipelined_rest.empty());
    run.join();
    RUVIA_CHECK(run.failure() == nullptr);
    RUVIA_CHECK(std::chrono::steady_clock::now() - stop_started < 10s);
}

// A tunnel has no in-band close signal, so a drain lets it run like an admitted
// request: it still carries data after the drain began and ends when its peer
// finishes, without waiting for the drain deadline.
RUVIA_TEST(app_drain_lets_tunnels_finish) {
    install_hooks();
    auto& app = ruvia::app();
    auto& state_value = state();
    state_value.tunnel_open_ = false;
    asio::io_context io;
    const auto endpoint = reserve_endpoint(io);
    app.server({.worker_count_ = 1, .drain_timeout_ = 30s, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    RUVIA_CHECK(state_value.started_.try_acquire_for(3s));
    // An idle keep-alive connection whose close shows that the drain began.
    asio::ip::tcp::socket idle(io);
    idle.connect(endpoint);
    send(idle, "GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
    std::string idle_response;
    RUVIA_CHECK(!read_until(idle, idle_response, [](const std::string& value) { return value.ends_with("fast"); }));

    asio::ip::tcp::socket tunnel(io);
    tunnel.connect(endpoint);
    send(tunnel, "CONNECT lifecycle-target.test:443 HTTP/1.1\r\nHost: lifecycle-target.test:443\r\n\r\n");
    std::string head;
    RUVIA_CHECK(!read_until(tunnel, head, [](const std::string& value) { return value.ends_with("\r\n\r\n"); }));
    RUVIA_CHECK(head.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(wait_for([&] { return state_value.tunnel_open_.load(); }));

    const auto stop_started = std::chrono::steady_clock::now();
    app.stop();
    std::string idle_rest;
    RUVIA_CHECK(read_to_end(idle, idle_rest, 3s));

    send(tunnel, "ping");
    std::string echoed;
    RUVIA_CHECK(!read_until(tunnel, echoed, [](const std::string& value) { return value.size() >= 4; }));
    RUVIA_CHECK_EQ(echoed, std::string("ping"));
    tunnel.shutdown(asio::ip::tcp::socket::shutdown_send);
    std::string rest;
    RUVIA_CHECK(read_to_end(tunnel, rest));
    RUVIA_CHECK(rest.empty());
    run.join();
    RUVIA_CHECK(run.failure() == nullptr);
    RUVIA_CHECK(std::chrono::steady_clock::now() - stop_started < 10s);
}

// Connection timeouts never fire early because of the scanner's coarse clock,
// even when the scan interval exceeds the timeout, and they still fire.
RUVIA_TEST(app_connection_timeouts_honor_coarse_scan_clock) {
    install_hooks();
    auto& app = ruvia::app();
    asio::io_context io;
    const auto endpoint = reserve_endpoint(io);
    constexpr auto header_timeout = 200ms;
    app.server({.worker_count_ = 1,
        .drain_timeout_ = 0ms,
        .connection_scan_interval_ = 300ms,
        .request_header_timeout_ = header_timeout});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});

    app_run run(app);
    RUVIA_CHECK(state().started_.try_acquire_for(3s));

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
    RUVIA_CHECK(!read_until(active, response, [](const std::string& value) { return value.ends_with("fast"); }));
    RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));

    // A connection that sends nothing is closed, but not before its timeout.
    const auto connected = std::chrono::steady_clock::now();
    asio::ip::tcp::socket silent(io);
    silent.connect(endpoint);
    std::string silent_bytes;
    RUVIA_CHECK(read_to_end(silent, silent_bytes, 5s));
    RUVIA_CHECK(std::chrono::steady_clock::now() - connected >= header_timeout);
    RUVIA_CHECK(silent_bytes.empty());

    app.stop();
    run.join();
    RUVIA_CHECK(run.failure() == nullptr);
}

#ifdef RUVIA_ENABLE_REDIS
// A stop while a worker capability is still connecting cancels startup: run()
// returns normally, and no worker handle was published before readiness.
RUVIA_TEST(app_stop_during_capability_connect_returns_normally) {
    install_hooks();
    auto& app = ruvia::app();
    asio::io_context io;
    // Accepts the Redis connection and never answers its AUTH.
    asio::ip::tcp::acceptor silent_redis(io, {asio::ip::address_v4::loopback(), 0});
    const auto endpoint = reserve_endpoint(io);
    app.server({.worker_count_ = 1});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    app.redis({.config_ = {.port_ = silent_redis.local_endpoint().port(),
                   .password_ = "secret",
                   .pool_size_per_worker_ = 1,
                   .connect_timeout_ = std::nullopt}});

    std::optional<app_run> run(std::in_place, app);
    silent_redis.non_blocking(true);
    asio::ip::tcp::socket peer(io);
    RUVIA_CHECK(wait_for([&] {
        asio::error_code error;
        silent_redis.accept(peer, error);
        return !error;
    }));
    std::string auth;
    RUVIA_CHECK(!read_until(peer, auth, [](const std::string& value) { return !value.empty(); }));
    RUVIA_CHECK(!auth.empty());
    RUVIA_CHECK(app.workers().empty());
    RUVIA_CHECK(!app.worker_for(std::uint64_t{0}).valid());

    app.stop();
    run->join();
    RUVIA_CHECK(run->failure() == nullptr);
    run.reset();
    app.redis(nullptr);
}
#endif
