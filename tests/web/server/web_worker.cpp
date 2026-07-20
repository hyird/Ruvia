#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <memory_resource>
#include <stdexcept>

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/Timer.h"
#include "ruvia/web/WebWorker.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServer.h"

namespace {

int testQueueFull() {
    ruvia::detail::RouteTable routes(std::pmr::get_default_resource());
    ruvia::detail::HttpServerOptions options;
    options.workerMailboxCapacity = 1;
    ruvia::detail::HttpServer server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
        routes,
        {},
        options);
    auto worker = server.webWorker();
    if (worker.post([](ruvia::WebWorkerContext&) -> ruvia::Task<void> {
            co_return;
        }) != ruvia::PostResult::kAccepted ||
        worker.post([](ruvia::WebWorkerContext&) -> ruvia::Task<void> {
            co_return;
        }) != ruvia::PostResult::kQueueFull) {
        return 1;
    }
    const auto queuedStats = worker.stats();
    if (queuedStats.accepted != 1 || queuedStats.queueFull != 1 ||
        queuedStats.outstanding != 1) {
        return 2;
    }
    server.start();
    server.stop();
    server.join();
    const auto drainedStats = worker.stats();
    if (drainedStats.completed != 1 || drainedStats.outstanding != 0) {
        return 3;
    }
    return 0;
}

int testFailureStopsWorker() {
    ruvia::detail::RouteTable routes(std::pmr::get_default_resource());
    ruvia::detail::HttpServer server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
        routes);
    auto worker = server.webWorker();
    server.start();
    if (worker.post([](ruvia::WebWorkerContext&) -> ruvia::Task<void> {
            throw std::runtime_error("web worker task failed");
            co_return;
        }) != ruvia::PostResult::kAccepted) {
        server.stop();
        server.join();
        return 1;
    }
    try {
        server.join();
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) == "web worker task failed" &&
            !worker.accepting() && worker.stats().failed == 1) {
            return 0;
        }
    }
    return 2;
}

int testGraceDeadlineCancelsTimer() {
    ruvia::detail::RouteTable routes(std::pmr::get_default_resource());
    ruvia::detail::HttpServerOptions options;
    options.shutdownGracePeriod = std::chrono::milliseconds(10);
    ruvia::detail::HttpServer server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
        routes,
        {},
        options);
    auto worker = server.webWorker();
    server.start();
    if (worker.post([](ruvia::WebWorkerContext& context) -> ruvia::Task<void> {
            static_cast<void>(
                co_await ruvia::sleepFor(
                    context.worker(), std::chrono::hours(1)));
        }) != ruvia::PostResult::kAccepted) {
        server.stop();
        server.join();
        return 1;
    }
    server.stop();
    server.join();
    const auto stats = worker.stats();
    return stats.completed == 1 && stats.outstanding == 0 ? 0 : 2;
}

int testGracefulDrain() {
    ruvia::detail::RouteTable routes(std::pmr::get_default_resource());
    ruvia::detail::HttpServerOptions options;
    options.shutdownGracePeriod = std::chrono::seconds(2);
    ruvia::detail::HttpServer server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
        routes,
        {},
        options);

    auto webWorker = server.webWorker();
    if (!webWorker.valid() || webWorker.id() == 0) {
        return 1;
    }

    server.start();
    std::promise<void> completed;
    auto completedFuture = completed.get_future();
    std::atomic_bool ranOnWorker{false};
    std::atomic_bool sawStop{false};
    auto moveOnly = std::make_unique<int>(42);

    const auto postResult = webWorker.post(
        [&, value = std::move(moveOnly)](
            ruvia::WebWorkerContext& context) -> ruvia::Task<void> {
            ranOnWorker.store(
                context.worker().isCurrent() &&
                    context.worker().id() == webWorker.id() &&
                    context.resource() != nullptr && *value == 42,
                std::memory_order_release);
            static_cast<void>(co_await ruvia::sleepFor(
                context.worker(), std::chrono::milliseconds(50)));
            sawStop.store(context.stopToken().stopRequested(),
                          std::memory_order_release);
            completed.set_value();
        });
    if (postResult != ruvia::PostResult::kAccepted) {
        server.stop();
        server.join();
        return 2;
    }

    server.stop();
    server.join();
    completedFuture.get();

    if (!ranOnWorker.load(std::memory_order_acquire) ||
        !sawStop.load(std::memory_order_acquire)) {
        return 3;
    }
    if (webWorker.accepting() ||
        webWorker.post([](ruvia::WebWorkerContext&) -> ruvia::Task<void> {
            co_return;
        }) != ruvia::PostResult::kWorkerStopping) {
        return 4;
    }
    return 0;
}

int testHandleOutlivesServerAsTerminalEndpoint() {
    ruvia::WebWorkerHandle worker;
    {
        ruvia::detail::RouteTable routes(std::pmr::get_default_resource());
        ruvia::detail::HttpServer server(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), 0),
            routes);
        worker = server.webWorker();
        server.start();
        server.stop();
        server.join();
    }

    const auto stats = worker.stats();
    return !worker.valid() && !worker.accepting() && worker.id() == 0 &&
            stats.outstanding == 0 &&
            worker.post([](ruvia::WebWorkerContext&) -> ruvia::Task<void> {
                co_return;
            }) == ruvia::PostResult::kWorkerStopping
        ? 0
        : 1;
}

// A raw mailbox task that throws synchronously, queued ahead of a WebWorker
// task, makes the dispatcher abandon the queued task when drain() rethrows. The
// abandoned task's outstanding_ reservation must still be reconciled: otherwise
// ~HttpServer's retire() sees a phantom in-flight task and std::terminate()s.
// Reaching the end of this function runs ~HttpServer; on the buggy code it
// aborts the process here.
int testAbandonedMailboxTaskReconciledOnThrow() {
    ruvia::detail::RouteTable routes(std::pmr::get_default_resource());
    ruvia::detail::HttpServerOptions options;
    options.workerMailboxCapacity = 4;
    ruvia::detail::HttpServer server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
        routes,
        {},
        options);
    auto worker = server.webWorker();

    if (server.worker().post([] {
            throw std::runtime_error("raw mailbox task threw");
        }) != ruvia::PostResult::kAccepted) {
        return 1;
    }
    if (worker.post([](ruvia::WebWorkerContext&) -> ruvia::Task<void> {
            co_return;
        }) != ruvia::PostResult::kAccepted) {
        return 2;
    }

    try {
        server.start();
    } catch (...) {
        // Startup fails because the mailbox task threw before the worker signaled
        // ready; the abandoned WebWorker task is what this test exercises.
    }
    try {
        server.join();
    } catch (...) {
    }
    return 0;
}

}  // namespace

int main() {
    if (testQueueFull() != 0) {
        return 1;
    }
    if (testFailureStopsWorker() != 0) {
        return 2;
    }
    if (testAbandonedMailboxTaskReconciledOnThrow() != 0) {
        return 6;
    }
    if (testGracefulDrain() != 0) {
        return 3;
    }
    if (testGraceDeadlineCancelsTimer() != 0) {
        return 4;
    }
    if (testHandleOutlivesServerAsTerminalEndpoint() != 0) {
        return 5;
    }
    return 0;
}
