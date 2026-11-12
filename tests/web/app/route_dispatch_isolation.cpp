#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string_view>

#include <asio/io_context.hpp>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/request_stream.h"
#include "ruvia/web/server_connection.h"

#include "app_run.h"
#include "raw_http_peer.h"
#include "tcp_port_reservation.h"

namespace route_dispatch_isolation_test {

using namespace std::chrono_literals;

std::atomic<int> controller_constructions{0};
std::atomic<int> middleware_constructions{0};
std::atomic<bool> reject_controller{true};
std::binary_semaphore started{0};

// Deliberately conflicting routes live in a separate executable: neither
// their registration nor their constructor belongs to non-routed listeners.
class conflicting_controller final {
public:
    conflicting_controller() {
        ++controller_constructions;
        if (reject_controller.load()) {
            throw std::runtime_error("unused controller constructed");
        }
    }

    RUVIA_ROUTES_BEGIN(conflicting_controller)
    RUVIA_GET("/conflict/:id", first);
    RUVIA_GET("/conflict/:name", second);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> first(ruvia::context& context) {
        co_return context.text("first");
    }

    ruvia::task<ruvia::http_response> second(ruvia::context& context) {
        co_return context.text("second");
    }
};

class unused_middleware final {
public:
    unused_middleware() {
        ++middleware_constructions;
        throw std::runtime_error("unused middleware constructed");
    }

    ruvia::task<void> handle(ruvia::context&, ruvia::next& next) {
        co_await next();
    }
};

ruvia::task<void> serve_connection(ruvia::server_connection& connection) {
    (void)co_await connection.read();
    constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\nconnection";
    co_await connection.write(std::as_bytes(std::span(response.data(), response.size())));
    co_await connection.finish();
}

ruvia::task<void> serve_request(ruvia::request_stream& request) {
    (void)co_await request.response().commit();
    co_await request.response().write(std::string_view("request-stream"));
    co_await request.response().end();
}

}  // namespace route_dispatch_isolation_test

using namespace route_dispatch_isolation_test;

TEST(Ruvia, nonrouted_listeners_ignore_conflicts_and_unused_controller_and_middleware_constructors) {
    asio::io_context io;
    auto& app = ruvia::app();
    controller_constructions = 0;
    middleware_constructions = 0;
    reject_controller = true;
    app.server({.worker_count_ = 2})
        .use<unused_middleware>()
        .on_connection(&serve_connection)
        .on_request_stream(&serve_request)
        .on_start(nullptr)
        .on_start([] { started.release(); });
    for (const auto dispatch : {ruvia::listener_dispatch::connection, ruvia::listener_dispatch::request_stream}) {
        ruvia::test::tcp_port_reservation reservation(io);
        const auto endpoint = reservation.endpoint();
        app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port(), .dispatch_ = dispatch});
        ruvia::test::app_run run(app);
        ASSERT_TRUE(started.try_acquire_for(10s));
        const auto response = ruvia::test::exchange(io, endpoint,
            "GET /conflict/1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
        EXPECT_NE(response.find(dispatch == ruvia::listener_dispatch::connection ? "connection" : "request-stream"), response.npos);
        app.stop();
        run.join();
    }
    EXPECT_EQ(controller_constructions.load(), 0);
    EXPECT_EQ(middleware_constructions.load(), 0);
}

TEST(Ruvia, mixed_routed_and_nonrouted_listeners_still_reject_conflicting_routes) {
    asio::io_context io;
    ruvia::test::tcp_port_reservation raw(io);
    ruvia::test::tcp_port_reservation routed(io);
    reject_controller = false;
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({
            {.address_ = "127.0.0.1", .http_ = raw.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection},
            {.address_ = "127.0.0.1", .http_ = routed.endpoint().port()},
        })
        .on_connection(&serve_connection)
        .on_start(nullptr);
    EXPECT_THROW(app.run(), std::invalid_argument);
}

TEST(Ruvia, adding_first_routed_listener_rejects_conflicts_without_interrupting_nonrouted_service) {
    asio::io_context io;
    ruvia::test::tcp_port_reservation raw(io);
    ruvia::test::tcp_port_reservation routed(io);
    reject_controller = false;
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .http_ = raw.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection})
        .on_connection(&serve_connection)
        .on_start(nullptr)
        .on_start([] { started.release(); });
    ruvia::test::app_run run(app);
    ASSERT_TRUE(started.try_acquire_for(10s));
    ruvia::test::wait_until([&app] {
        try {
            app.remove_listener({.address_ = "0.0.0.0", .port_ = 9});
        } catch (const std::invalid_argument&) {
            return true;
        } catch (const std::logic_error&) {
        }
        return false;
    },
        10s);
    EXPECT_THROW(app.add_listener({.address_ = "127.0.0.1", .http_ = routed.endpoint().port()}), std::invalid_argument);
    EXPECT_TRUE(ruvia::test::exchange(io, raw.endpoint(),
        "GET /still-serving HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
            .ends_with("connection"));
    app.stop();
    run.join();
}
