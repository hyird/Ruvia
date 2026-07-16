// A configured shutdownGracePeriod must delay force-close only while sessions
// are still draining. Case 1: stopping an idle server joins immediately.
// Case 2: the force-close fires as soon as the last session ends, not after
// the full grace period. Case 3: worker failure overrides an already-posted
// graceful stop. Each case must finish within the generous bound.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServer.h"

namespace {

constexpr auto kGracePeriod = std::chrono::seconds(30);
constexpr auto kJoinBound = std::chrono::seconds(10);

ruvia::detail::HttpServerOptions gracefulOptions() {
    ruvia::detail::HttpServerOptions options;
    options.shutdownGracePeriod = kGracePeriod;
    return options;
}

[[nodiscard]] std::chrono::steady_clock::duration stopAndJoin(
    ruvia::detail::HttpServer& server) {
    server.stop();
    const auto begin = std::chrono::steady_clock::now();
    server.join();
    return std::chrono::steady_clock::now() - begin;
}

}  // namespace

int main() {
    std::pmr::memory_resource* resource = std::pmr::get_default_resource();

    {
        // Case 1: no connections. The drain timer must not arm at all.
        ruvia::detail::RouteTable routes(resource);
        ruvia::detail::HttpServer server(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
            routes,
            {},
            gracefulOptions());
        server.start();
        if (stopAndJoin(server) >= kJoinBound) {
            std::fputs("idle shutdown waited on the grace period\n", stderr);
            return 1;
        }
    }

    {
        // Case 2: one keep-alive session outlives stop(). Closing it must
        // finish the drain early instead of waiting out the grace period.
        ruvia::detail::RouteTable routes(resource);
        ruvia::detail::HttpServer server(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
            routes,
            {},
            gracefulOptions());
        server.start();

        asio::io_context clientContext;
        asio::ip::tcp::socket client(clientContext);
        client.connect(server.localEndpoint());
        constexpr std::string_view request =
            "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(client, asio::buffer(request));
        asio::streambuf response;
        asio::read_until(client, response, "\r\n\r\n");

        server.stop();
        // Let the posted stopOnContext run while the session still exists, so
        // this case exercises the armed drain timer rather than the idle path.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // The response above proves the session is counted, so stop() has
        // armed the drain timer. Ending the session must release it.
        std::error_code ignored;
        client.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        client.close(ignored);

        const auto begin = std::chrono::steady_clock::now();
        server.join();
        if (std::chrono::steady_clock::now() - begin >= kJoinBound) {
            std::fputs("drained shutdown waited on the grace period\n", stderr);
            return 2;
        }
    }

    {
        // Case 3: a worker failure may race with the already-posted graceful
        // stop. The forced failure path must make the later graceful callback
        // a no-op instead of trying to arm a timer after timers were stopped.
        ruvia::detail::RouteTable routes(resource);
        ruvia::detail::HttpServer server(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
            routes,
            {},
            gracefulOptions());
        server.start();

        asio::io_context clientContext;
        asio::ip::tcp::socket client(clientContext);
        client.connect(server.localEndpoint());
        constexpr std::string_view request =
            "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(client, asio::buffer(request));
        asio::streambuf response;
        asio::read_until(client, response, "\r\n\r\n");

        std::promise<void> workerEntered;
        auto workerEnteredFuture = workerEntered.get_future();
        std::atomic_bool releaseWorker{false};
        if (server.webWorker().post(
                [&](ruvia::WebWorkerContext&) -> ruvia::Task<void> {
                    workerEntered.set_value();
                    while (!releaseWorker.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    throw std::runtime_error("worker failed during graceful stop");
                    co_return;
                }) != ruvia::PostResult::kAccepted) {
            return 3;
        }

        workerEnteredFuture.wait();
        server.stop();
        releaseWorker.store(true, std::memory_order_release);
        const auto begin = std::chrono::steady_clock::now();
        bool sawWorkerFailure = false;
        try {
            server.join();
        } catch (const std::runtime_error& error) {
            sawWorkerFailure = std::string_view(error.what()) ==
                "worker failed during graceful stop";
        }

        std::error_code ignored;
        client.close(ignored);
        if (!sawWorkerFailure ||
            std::chrono::steady_clock::now() - begin >= kJoinBound) {
            std::fputs("worker failure did not override graceful shutdown\n", stderr);
            return 4;
        }
    }

    return 0;
}
