#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3MessageHead.h"
#include "ruvia/http/Http3ServerRequest.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/DocumentRootBinding.h"

#include "context_services_fixture.h"
#include "routing_fixture.h"
#include "test_harness.h"

namespace {

ruvia::Task<ruvia::HttpResponse> routeHttp3Request(void*, ruvia::Context& context) {
    const auto body = co_await context.req().text();
    const bool valid = context.req().method() == "POST" && context.req().path() == "/items" &&
                       context.req().header("Host") == "example.test" &&
                       context.req().cookie("session") == "one" &&
                       context.req().cookie("other") == "two" && body == "payload";
    co_return context.text(valid ? std::string_view("h3-route-ok")
                                 : std::string_view("h3-route-mismatch"));
}

}  // namespace

RUVIA_TEST(http3RequestResolvesAtHeadersAndDispatchesAfterIncrementalBodyCompletion) {
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    implementation.registerRoute(ruvia::HttpKnownMethod::kPost, routing_test::path("/items"),
        ruvia::detail::RouteHandler(nullptr, &routeHttp3Request),
        ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    implementation.finalize();
    const auto& routes = implementation.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    std::optional<ruvia::Http3ServerRequest> owner;
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{"cookie", "session=one"},
        ruvia::Http3FieldSectionFieldView{"cookie", "other=two"}};
    const auto wire = ruvia::encodeHttp3ClientRequestHead({.method = "POST", .scheme = "https", .authority = "example.test", .path = "/items", .fields = fields, .bodyLength = 7});
    RUVIA_CHECK(wire.has_value());
    if (!wire) {
        return;
    }
    {
        auto decoded = ruvia::decodeHttp3MessageHead(wire->fieldSection,
            ruvia::Http3MessageHeadKind::kRequest, worker.resource());
        RUVIA_CHECK(decoded.has_value());
        if (!decoded) {
            return;
        }
        owner.emplace(*decoded, memory.resource(), memory.upstreamResource());
    }
    const auto& request = owner->request();
    RUVIA_CHECK(!owner->bodyComplete());
    RUVIA_CHECK(request.bodyBytes().empty());
    const auto resolution = routes.resolve(request);
    RUVIA_CHECK(resolution.resolved() != nullptr);
    if (resolution.resolved() == nullptr) {
        return;
    }

    std::array partOne{std::byte{'p'}, std::byte{'a'}, std::byte{'y'}};
    std::array partTwo{std::byte{'l'}, std::byte{'o'}, std::byte{'a'}, std::byte{'d'}};
    owner->appendBody(partOne);
    owner->appendBody(partTwo);
    RUVIA_CHECK(request.bodyBytes().empty());
    owner->finishBody();
    partOne.fill(std::byte{'x'});
    partTwo.fill(std::byte{'x'});
    RUVIA_CHECK(owner->bodyComplete());

    asio::io_context io(1);
    auto future = asio::co_spawn(io, ruvia::asAwaitable(routes.dispatchBufferedResponse(request, resolution, memory, ruvia::detail::DocumentRootBinding::none(), ruvia::test::testContextServices())), asio::use_future);
    io.run();
    const auto response = future.get();
    RUVIA_CHECK(response.status() == ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(response.bodyBytes(), std::string_view("h3-route-ok"));
}

RUVIA_TEST(http3RequestUsesNormalWebBufferedRouteAndOwnsBodyThroughDispatch) {
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    implementation.registerRoute(ruvia::HttpKnownMethod::kPost, routing_test::path("/items"),
        ruvia::detail::RouteHandler(nullptr, &routeHttp3Request),
        ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    implementation.finalize();
    const auto& routes = implementation.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{"cookie", "session=one"},
        ruvia::Http3FieldSectionFieldView{"cookie", "other=two"}};
    const auto wire = ruvia::encodeHttp3ClientRequestHead({.method = "POST", .scheme = "https", .authority = "example.test", .path = "/items", .fields = fields, .bodyLength = 7});
    RUVIA_CHECK(wire.has_value());
    if (!wire) {
        return;
    }
    auto decoded = ruvia::decodeHttp3MessageHead(wire->fieldSection,
        ruvia::Http3MessageHeadKind::kRequest, memory.resource());
    RUVIA_CHECK(decoded.has_value());
    if (!decoded) {
        return;
    }

    std::array body{std::byte{'p'}, std::byte{'a'}, std::byte{'y'}, std::byte{'l'},
        std::byte{'o'}, std::byte{'a'}, std::byte{'d'}};
    ruvia::Http3ServerRequest owner(*decoded, memory.resource(), memory.upstreamResource());
    owner.appendBody(body);
    owner.finishBody();
    body.fill(std::byte{'x'});  // The handler must not borrow the caller's transport buffer.
    const auto& request = owner.request();
    RUVIA_CHECK(request.protocolVersion() == ruvia::HttpProtocolVersion::kHttp3);
    RUVIA_CHECK(request.targetForm() == ruvia::HttpRequestTargetForm::kHttp3);
    const auto resolution = routes.resolve(request);
    RUVIA_CHECK(resolution.resolved() != nullptr);
    if (resolution.resolved() == nullptr) {
        return;
    }

    asio::io_context io(1);
    auto future = asio::co_spawn(io, ruvia::asAwaitable(routes.dispatchBufferedResponse(request, resolution, memory, ruvia::detail::DocumentRootBinding::none(), ruvia::test::testContextServices())), asio::use_future);
    io.run();
    const auto response = future.get();
    RUVIA_CHECK(response.status() == ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(response.bodyBytes(), std::string_view("h3-route-ok"));
}
