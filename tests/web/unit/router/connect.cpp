#include "ruvia/web/detail/router/RouterImpl.h"

#include "routing_fixture.h"

namespace {
ruvia::Task<void> connectHandler(void*, ruvia::Context&) {
    co_return;
}
void registerConnectRoutes(ruvia::detail::RouterImpl& router) {
    const ruvia::detail::RouteStreamHandler handler(nullptr, &connectHandler);
    router.registerTunnelRoute({}, std::pmr::string("proxy.test:443"), handler, {}, {});
    router.registerTunnelRoute({}, std::pmr::string("*"), handler, {}, {});
    router.registerTunnelRoute("connect-udp", std::pmr::string("/udp/:host/:port"), handler, {}, {});
    router.registerTunnelRoute("test-protocol", std::pmr::string("/udp/:host/:port"), handler, {}, {});
    router.registerTunnelRoute("connect-udp", std::pmr::string("/udp/static/443"), handler, {}, {});
}
}  // namespace
RUVIA_TEST(connectRoutesMatchAuthoritiesAndSeparateProtocolPathsWithSharedWorkerPlan) {
    ruvia::detail::Router firstRouter;
    auto& first = ruvia::detail::RouterImpl::from(firstRouter);
    registerConnectRoutes(first);
    first.finalize();
    auto plan = first.releaseCompiledPlan();
    ruvia::detail::Router secondRouter;
    auto& second = ruvia::detail::RouterImpl::from(secondRouter);
    registerConnectRoutes(second);
    second.finalize(plan.get());
    const auto& table = second.routeTable();
    auto exact = table.resolve(ruvia::HttpKnownMethod::kConnect, "PROXY.TEST:0443");
    RUVIA_CHECK(exact.resolved() != nullptr);
    RUVIA_CHECK(exact.resolved()->route().path() == "proxy.test:443");
    auto fallback = table.resolve(ruvia::detail::route_request_view{ruvia::HttpKnownMethod::kConnect, "CONNECT", {}, "other.test:80", {}});
    RUVIA_CHECK(fallback.resolved() != nullptr);
    RUVIA_CHECK(fallback.resolved()->route().path() == "*");
    auto udp = table.resolve(ruvia::detail::route_request_view{ruvia::HttpKnownMethod::kConnect, "CONNECT", "/udp/target.test/443", "ignored.test:443", "connect-udp"});
    RUVIA_CHECK(udp.resolved() != nullptr);
    RUVIA_CHECK(udp.resolved()->route().endpoint().tunnel()->protocol() == "connect-udp");
    RUVIA_CHECK_EQ(udp.resolved()->match().values().size(), std::size_t{2});
    RUVIA_CHECK(udp.resolved()->match().values()[0] == "target.test");
    RUVIA_CHECK(udp.resolved()->match().values()[1] == "443");
    auto staticRoute = table.resolve(ruvia::detail::route_request_view{ruvia::HttpKnownMethod::kConnect, "CONNECT", "/udp/static/443", "ignored.test:443", "connect-udp"});
    RUVIA_CHECK(staticRoute.resolved() != nullptr);
    RUVIA_CHECK(staticRoute.resolved()->match().size() == 0);
    const auto unknown = table.resolve(ruvia::detail::route_request_view{ruvia::HttpKnownMethod::kConnect, "CONNECT", "/udp/target.test/443", "proxy.test:443", "unregistered"});
    RUVIA_CHECK(unknown.notFound() != nullptr);
    const auto ordinary = table.resolve(ruvia::HttpKnownMethod::kGet, "/udp/target.test/443");
    RUVIA_CHECK(ordinary.notFound() != nullptr);
}
RUVIA_TEST(connectRoutesRejectInvalidAuthoritiesAndEquivalentDuplicateTargets) {
    const ruvia::detail::RouteStreamHandler handler(nullptr, &connectHandler);
    for (const auto authority : {"proxy.test", "proxy.test:", "proxy.test:65536", "/proxy", "user@proxy.test:443"}) {
        ruvia::detail::Router router;
        auto& routes = ruvia::detail::RouterImpl::from(router);
        bool rejected = false;
        try {
            routes.registerTunnelRoute({}, std::pmr::string(authority), handler, {}, {});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
    ruvia::detail::Router router;
    auto& routes = ruvia::detail::RouterImpl::from(router);
    routes.registerTunnelRoute({}, std::pmr::string("proxy.test:443"), handler, {}, {});
    bool rejected = false;
    try {
        routes.registerTunnelRoute({}, std::pmr::string("PROXY.TEST:0443"), handler, {}, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(route_facts_keep_exact_methods_and_extended_connect_targets_separate) {
    ruvia::detail::Router router;
    auto& routes = ruvia::detail::RouterImpl::from(router);
    registerConnectRoutes(routes);
    routes.registerExtensionMethodRoute("MiXeD", std::pmr::string("/extension"),
        ruvia::detail::RouteHandler(nullptr, &routing_test::dummyHandler),
        ruvia::detail::RequestBodyMode::kStream, {}, {});
    routes.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/socket"),
        ruvia::detail::RouteStreamHandler(nullptr, &connectHandler), {}, {}, {});
    routes.registerTunnelRoute("WebSocket", std::pmr::string("/socket"),
        ruvia::detail::RouteStreamHandler(nullptr, &connectHandler), {}, {});
    routes.finalize();
    const auto& table = routes.routeTable();
    using facts = ruvia::detail::route_request_view;
    const auto extension = table.resolve(facts{ruvia::HttpKnownMethod::kUnknown, "MiXeD", "/extension", {}, {}});
    RUVIA_CHECK(extension.resolved() != nullptr);
    RUVIA_CHECK(extension.resolved()->route().endpoint().requestBodyMode() == ruvia::detail::RequestBodyMode::kStream);
    const auto different_case = table.resolve(facts{ruvia::HttpKnownMethod::kUnknown, "mixed", "/extension", {}, {}});
    RUVIA_CHECK(different_case.notFound() != nullptr);
    const auto socket = table.resolve(facts{ruvia::HttpKnownMethod::kConnect, "CONNECT", "/socket", "proxy.test:443", "websocket"});
    RUVIA_CHECK(socket.resolved() != nullptr);
    RUVIA_CHECK(socket.resolved()->route().endpoint().webSocket() != nullptr);
    const auto raw_protocol = table.resolve(facts{ruvia::HttpKnownMethod::kConnect, "CONNECT", "/socket", "proxy.test:443", "WeBsOcKeT"});
    RUVIA_CHECK(raw_protocol.notFound() != nullptr);
    const auto exact_protocol = table.resolve(facts{ruvia::HttpKnownMethod::kConnect, "CONNECT", "/socket", "proxy.test:443", "WebSocket"});
    RUVIA_CHECK(exact_protocol.resolved() != nullptr);
    RUVIA_CHECK(exact_protocol.resolved()->route().endpoint().tunnel() != nullptr);
    RUVIA_CHECK(exact_protocol.resolved()->route().endpoint().webSocket() == nullptr);
    const auto authority = table.resolve(facts{ruvia::HttpKnownMethod::kConnect, "CONNECT", "/socket", "PROXY.TEST:0443", {}});
    RUVIA_CHECK(authority.resolved() != nullptr);
    RUVIA_CHECK_EQ(authority.resolved()->route().path(), std::string_view("proxy.test:443"));
    const auto server_options = table.resolve(facts{ruvia::HttpKnownMethod::kOptions, "OPTIONS", "*", {}, {}});
    RUVIA_CHECK(server_options.resolved() == nullptr);
}
