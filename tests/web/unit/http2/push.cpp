#include <array>
#include <chrono>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/web/HttpClient.h"

#include "http2/Http2SansIoSession.h"
#include "http2_sansio_session_fixture.h"
#include "memory_resource_fixture.h"
#include "router/Router.h"
#include "router/RouterImpl.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
ruvia::Task<std::string> collectText(ruvia::HttpClientResponse& response) {
    const auto bytes = co_await response.body().readAll();
    const auto view = bytes.bytes();
    co_return std::string(reinterpret_cast<const char*>(view.data()), view.size());
}
struct PushRoutes final {
    unsigned promised{};
    unsigned refused{};
    unsigned streamed{};
    static ruvia::Task<ruvia::HttpResponse> parent(void* raw, ruvia::Context& context) {
        auto& owner = *static_cast<PushRoutes*>(raw);
        const auto count = context.req().path() == "/two" ? 2U : 1U;
        const auto path = context.req().path() == "/large" ? "/large-asset" : "/asset";
        const std::array<ruvia::HttpHeaderView, 1> headers{{{"x-promise", "owned-request"}}};
        for (unsigned i = 0; i != count; ++i) {
            if (co_await context.push({.scheme = "http", .authority = context.req().authority(), .path = path, .headers = headers})) {
                ++owner.promised;
            } else {
                ++owner.refused;
            }
        }
        co_return context.text("parent");
    }
    static ruvia::Task<ruvia::HttpResponse> asset(void*, ruvia::Context& context) {
        if (context.req().header("x-promise") != "owned-request") {
            throw std::runtime_error("lost push request metadata");
        }
        context.header("x-asset", "metadata");
        co_return context.text("pushed-body");
    }
    static ruvia::Task<void> large(void* raw, ruvia::Context& context) {
        auto& owner = *static_cast<PushRoutes*>(raw);
        std::string chunk(16384, 'p');
        for (unsigned i = 0; i != 80; ++i) {
            co_await context.stream().write(chunk);
        }
        co_await context.stream().end();
        ++owner.streamed;
    }
    void registerWith(ruvia::detail::RouterImpl& routes) {
        for (const auto path : {"/parent", "/two", "/large"}) {
            routes.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string(path),
                ruvia::detail::RouteHandler(this, &parent), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
        }
        routes.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/asset"),
            ruvia::detail::RouteHandler(this, &asset), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
        routes.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/large-asset"),
            ruvia::detail::RouteStreamHandler(this, &large), {}, {});
        routes.finalize();
    }
};

ruvia::Task<void> serve(asio::ip::tcp::acceptor& acceptor, const ruvia::WorkerHandle& worker,
    const ruvia::detail::RouteTable& routes, ruvia::WorkerMemory& memory) {
    auto accepted = co_await ruvia::asyncAsio<asio::ip::tcp::socket>([&](auto completion) {
        acceptor.async_accept(std::move(completion));
    });
    if (accepted.errorCode()) {
        throw std::system_error(accepted.errorCode());
    }
    auto socket = std::move(accepted.result());
    ruvia::test::Http2SansIoSessionFixture fixture;
    co_await ruvia::detail::runHttp2SansIoSession(socket, routes, memory,
        fixture.context(fixture.services(worker).withPlainTransport("127.0.0.1")));
}
}  // namespace

RUVIA_TEST(http2_push_routes_and_client_preserve_owners_flow_control_cold_reads_and_shutdown) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    ruvia::test::CountingMemoryResource allocationUpstream;
    auto run = [&]() -> ruvia::Task<void> {
        const auto& worker = attachment.loop().handle();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        ruvia::WorkerMemory memory(allocationUpstream);
        ruvia::detail::Router router;
        auto& routes = ruvia::detail::RouterImpl::from(router);
        PushRoutes observation;
        observation.registerWith(routes);
        ruvia::TaskScope tasks(worker);
        tasks.spawn(serve(acceptor, worker, routes.routeTable(), memory));
        std::optional<ruvia::HttpClientPush> retained;
        std::optional<ruvia::HttpClientResponse> retainedResponse;
        {
            ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttp,
                                                            .host = "127.0.0.1",
                                                            .port = acceptor.local_endpoint().port(),
                                                            .protocol = ruvia::HttpClientProtocol::kHttp2Only,
                                                            .push = {.enabled = true}});
            std::exception_ptr failure;
            try {
                std::size_t warmAllocations{};
                for (unsigned repeat = 0; repeat != 128; ++repeat) {
                    auto parent = co_await client.send({.target = "/parent"});
                    RUVIA_CHECK((co_await collectText(parent)) == "parent");
                    auto push = client.nextPush();
                    RUVIA_CHECK(push.has_value());
                    if (!push) {
                        throw std::runtime_error("missing push");
                    }
                    RUVIA_CHECK(push->request().path == "/asset");
                    RUVIA_CHECK(push->request().headers.front().value() == "owned-request");
                    {
                        auto cold = push->response();
                    }
                    auto pending = push->response();
                    bool busy = false;
                    try {
                        auto overlap = push->response();
                    } catch (const std::logic_error&) {
                        busy = true;
                    }
                    RUVIA_CHECK(busy);
                    auto moved = std::move(*push);
                    auto response = co_await std::move(pending);
                    RUVIA_CHECK(response.status() == ruvia::http_status::kOk);
                    RUVIA_CHECK(response.header("x-asset") == "metadata");
                    RUVIA_CHECK((co_await collectText(response)) == "pushed-body");
                    RUVIA_CHECK(!client.nextPush());
                    if (repeat == 0) {
                        retained.emplace(std::move(moved));
                        retainedResponse.emplace(std::move(response));
                    }
                    RUVIA_CHECK(retained->request().path == "/asset");
                    RUVIA_CHECK(retainedResponse->header("x-asset") == "metadata");
                    if (repeat == 96) {
                        warmAllocations = allocationUpstream.allocationCount();
                    }
                    if (repeat > 96) {
                        RUVIA_CHECK_EQ(allocationUpstream.allocationCount(), warmAllocations);
                    }
                }
                auto parent = co_await client.send({.target = "/large"});
                RUVIA_CHECK((co_await collectText(parent)) == "parent");
                auto pushed = client.nextPush();
                RUVIA_CHECK(pushed.has_value());
                if (!pushed) {
                    throw std::runtime_error("missing large push");
                }
                auto response = co_await pushed->response();
                std::size_t bytes = 0;
                while (auto chunk = co_await response.body().text()) {
                    RUVIA_CHECK(chunk->find_first_not_of('p') == std::string_view::npos);
                    bytes += chunk->size();
                }
                RUVIA_CHECK_EQ(bytes, std::size_t{80 * 16384});
                RUVIA_CHECK_EQ(observation.streamed, 1U);
                auto cancelledParent = co_await client.send({.target = "/large"});
                RUVIA_CHECK((co_await collectText(cancelledParent)) == "parent");
                auto cancelledPush = client.nextPush();
                RUVIA_CHECK(cancelledPush.has_value());
                cancelledPush.reset();
                auto afterCancel = co_await client.send({.target = "/parent"});
                RUVIA_CHECK((co_await collectText(afterCancel)) == "parent");
                auto afterPush = client.nextPush();
                RUVIA_CHECK(afterPush.has_value());
                if (afterPush) {
                    auto afterResponse = co_await afterPush->response();
                    RUVIA_CHECK((co_await collectText(afterResponse)) == "pushed-body");
                }
                RUVIA_CHECK_EQ(client.stats().receivedPushes, std::size_t{131});
                RUVIA_CHECK_EQ(client.stats().rejectedPushes, std::size_t{0});
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            co_await tasks.join();
            if (failure) {
                std::rethrow_exception(failure);
            }
        }
        RUVIA_CHECK(retained->request().path == "/asset");
        RUVIA_CHECK(retainedResponse->header("x-asset") == "metadata");
        retainedResponse.reset();
        retained.reset();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    RUVIA_CHECK_EQ(allocationUpstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(allocationUpstream.allocationCount(), allocationUpstream.deallocationCount());
}

RUVIA_TEST(http2_push_queue_overflow_and_disabled_permission_keep_parent_response_usable) {
    for (const bool enabled : {false, true}) {
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io);
        auto run = [&]() -> ruvia::Task<void> {
            const auto& worker = attachment.loop().handle();
            asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
            ruvia::WorkerMemory memory;
            ruvia::detail::Router router;
            auto& routes = ruvia::detail::RouterImpl::from(router);
            PushRoutes observation;
            observation.registerWith(routes);
            ruvia::TaskScope tasks(worker);
            tasks.spawn(serve(acceptor, worker, routes.routeTable(), memory));
            ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttp,
                                                            .host = "127.0.0.1",
                                                            .port = acceptor.local_endpoint().port(),
                                                            .protocol = ruvia::HttpClientProtocol::kHttp2Only,
                                                            .push = {.enabled = enabled, .maxQueuedPushes = 1}});
            std::exception_ptr failure;
            try {
                auto parent = co_await client.send({.target = "/two"});
                RUVIA_CHECK((co_await collectText(parent)) == "parent");
                auto push = client.nextPush();
                RUVIA_CHECK(push.has_value() == enabled);
                if (push) {
                    auto response = co_await push->response();
                    RUVIA_CHECK((co_await collectText(response)) == "pushed-body");
                }
                RUVIA_CHECK_EQ(client.stats().receivedPushes, enabled ? std::size_t{1} : std::size_t{0});
                RUVIA_CHECK_EQ(client.stats().rejectedPushes, enabled ? std::size_t{1} : std::size_t{0});
                RUVIA_CHECK_EQ(observation.refused, enabled ? 0U : 2U);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            co_await tasks.join();
            if (failure) {
                std::rethrow_exception(failure);
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        root.get();
    }
}
