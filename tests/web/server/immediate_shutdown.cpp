// Web shutdown is immediate: stop the listener, active sockets, integrations,
// worker tasks, and timers without waiting for request draining. Case 1 covers
// an idle server, case 2 an idle keep-alive socket, and case 3 a worker failure
// racing with the already-posted stop callback.

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
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

namespace {

constexpr auto kJoinBound = std::chrono::seconds(3);

[[nodiscard]] std::chrono::steady_clock::duration stopAndJoin(ruvia::detail::WebWorkerRuntime& server) {
    server.stop();
    const auto begin = std::chrono::steady_clock::now();
    server.join();
    return std::chrono::steady_clock::now() - begin;
}

}  // namespace

int main() {
    std::pmr::memory_resource* resource = std::pmr::get_default_resource();

    {
        // Case 1: no connections.
        ruvia::detail::RouteTable routes(resource);
        ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routes);
        server.start();
        if (stopAndJoin(server) >= kJoinBound) {
            std::fputs("idle shutdown did not finish immediately\n", stderr);
            return 1;
        }
    }

    {
        // Case 2: stop() owns socket termination. It must not wait for an idle
        // keep-alive client to close its side of the connection.
        ruvia::detail::RouteTable routes(resource);
        ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routes);
        server.start();

        asio::io_context clientContext;
        asio::ip::tcp::socket client(clientContext);
        client.connect(server.localEndpoint());
        constexpr std::string_view request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(client, asio::buffer(request));
        asio::streambuf response;
        asio::read_until(client, response, "\r\n\r\n");

        if (stopAndJoin(server) >= kJoinBound) {
            std::fputs("idle keep-alive delayed shutdown\n", stderr);
            return 2;
        }
    }

    {
        // Case 3: a worker failure may race with an already-posted stop. Both
        // paths must converge on the same immediate terminal state.
        ruvia::detail::RouteTable routes(resource);
        ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routes);
        server.start();

        asio::io_context clientContext;
        asio::ip::tcp::socket client(clientContext);
        client.connect(server.localEndpoint());
        constexpr std::string_view request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(client, asio::buffer(request));
        asio::streambuf response;
        asio::read_until(client, response, "\r\n\r\n");

        std::promise<void> workerEntered;
        auto workerEnteredFuture = workerEntered.get_future();
        std::atomic_bool releaseWorker{false};
        if (server.webWorker().post([&](ruvia::WebWorkerContext&) -> ruvia::Task<void> {
                workerEntered.set_value();
                while (!releaseWorker.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                throw std::runtime_error("worker failed during immediate stop");
                co_return;
            }) != ruvia::PostStatus::kAccepted) {
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
            sawWorkerFailure = std::string_view(error.what()) == "worker failed during immediate stop";
        }

        std::error_code ignored;
        client.close(ignored);
        if (!sawWorkerFailure || std::chrono::steady_clock::now() - begin >= kJoinBound) {
            std::fputs("worker failure race delayed immediate shutdown\n", stderr);
            return 4;
        }
    }

    return 0;
}
