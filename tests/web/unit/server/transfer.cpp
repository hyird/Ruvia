#include <chrono>
#include <span>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

#include "test_harness.h"

RUVIA_TEST(validated_web_worker_runs_without_binding_listeners) {
    using Listener = ruvia::detail::HttpServerListenerDefinition;
    asio::io_context reservationContext;
    asio::ip::tcp::acceptor reservation(reservationContext, asio::ip::tcp::v4());
    reservation.bind({asio::ip::address_v4::loopback(), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();

    Listener listener(endpoint);
    ruvia::detail::HttpServerOptions options;
    auto configuration = ruvia::detail::validateHttpServerConfiguration(
        std::span<const Listener>(&listener, 1), std::move(options));
    ruvia::detail::RouteTable routes(std::pmr::get_default_resource());
    ruvia::detail::WebWorkerRuntime runtime(configuration, routes, {});
    runtime.prepare();

    asio::ip::tcp::acceptor probe(runtime.workerExecutor());
    probe.open(endpoint.protocol());
    probe.bind(endpoint);

    runtime.launch();
    runtime.waitUntilReady();
    runtime.requestServe();
    RUVIA_CHECK(runtime.waitUntilServing());
    runtime.stop();
    runtime.join();
}

RUVIA_TEST(web_worker_records_transferred_socket_assignment_failure) {
    using namespace std::chrono_literals;

    ruvia::detail::RouteTable routes(std::pmr::get_default_resource());
    ruvia::detail::WebWorkerRuntime runtime(
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), routes, {});
    runtime.start();

    auto ticket = ruvia::detail::NativeAcceptedSocketTicket(asio::ip::tcp::v4(), 0,
        ruvia::detail::NativeAcceptedSocketTicket::invalidNative());
    auto post = runtime.worker().post([&runtime, ticket = std::move(ticket)]() mutable {
        runtime.acceptTransferredConnection(std::move(ticket));
    });
    RUVIA_CHECK(post.accepted());

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (runtime.stats().acceptFailures == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(runtime.stats().acceptFailures == 1U);
    RUVIA_CHECK(runtime.stats().workerFailures == 0U);
    runtime.stop();
    runtime.join();
}

RUVIA_TEST(web_worker_accepts_transferred_connection_on_its_worker) {
    using namespace std::chrono_literals;

    ruvia::detail::RouteTable routes(std::pmr::get_default_resource());
    ruvia::detail::HttpServerOptions options;
    options.maxConnections = 1;
    ruvia::detail::WebWorkerRuntime runtime(
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), routes, {}, options);
    runtime.start();
    RUVIA_CHECK(runtime.availableForIngress());

    asio::ip::tcp::acceptor source(runtime.workerExecutor(),
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    asio::ip::tcp::socket firstClient(runtime.workerExecutor());
    firstClient.connect(source.local_endpoint());
    asio::ip::tcp::socket first(runtime.workerExecutor());
    source.accept(first);
    asio::error_code releaseError;
    auto firstNative = first.release(releaseError);
    RUVIA_CHECK(!releaseError);
    auto firstTicket = ruvia::detail::NativeAcceptedSocketTicket(
        asio::ip::tcp::v4(), 0, firstNative);
    auto firstPost = runtime.worker().post([&runtime, firstTicket = std::move(firstTicket)]() mutable {
        runtime.acceptTransferredConnection(std::move(firstTicket));
    });
    RUVIA_CHECK(firstPost.accepted());

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (runtime.stats().activeConnections == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(runtime.stats().activeConnections == 1U);
    RUVIA_CHECK(!runtime.availableForIngress());

    asio::ip::tcp::socket secondClient(runtime.workerExecutor());
    secondClient.connect(source.local_endpoint());
    asio::ip::tcp::socket second(runtime.workerExecutor());
    source.accept(second);
    auto secondNative = second.release(releaseError);
    RUVIA_CHECK(!releaseError);
    auto secondTicket = ruvia::detail::NativeAcceptedSocketTicket(
        asio::ip::tcp::v4(), 0, secondNative);
    auto secondPost = runtime.worker().post([&runtime, secondTicket = std::move(secondTicket)]() mutable {
        runtime.acceptTransferredConnection(std::move(secondTicket));
    });
    RUVIA_CHECK(secondPost.accepted());
    while (runtime.stats().connectionsRefused == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(runtime.stats().connectionsRefused >= 1U);
    runtime.stop();
    runtime.join();

    RUVIA_CHECK(runtime.worker().post([] {}).status() == ruvia::PostStatus::kWorkerStopping);
}
