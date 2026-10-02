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
    auto fallback = table.resolveConnect({}, "other.test:80");
    RUVIA_CHECK(fallback.resolved() != nullptr);
    RUVIA_CHECK(fallback.resolved()->route().path() == "*");
    auto udp = table.resolveConnect("connect-udp", "/udp/target.test/443");
    RUVIA_CHECK(udp.resolved() != nullptr);
    RUVIA_CHECK(udp.resolved()->route().endpoint().tunnel()->protocol() == "connect-udp");
    RUVIA_CHECK_EQ(udp.resolved()->match().values().size(), std::size_t{2});
    RUVIA_CHECK(udp.resolved()->match().values()[0] == "target.test");
    RUVIA_CHECK(udp.resolved()->match().values()[1] == "443");
    auto staticRoute = table.resolveConnect("connect-udp", "/udp/static/443");
    RUVIA_CHECK(staticRoute.resolved() != nullptr);
    RUVIA_CHECK(staticRoute.resolved()->match().size() == 0);
    const auto unknown = table.resolveConnect("unregistered", "/udp/target.test/443");
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
