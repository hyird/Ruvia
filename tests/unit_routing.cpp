#include "test_harness.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <exception>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "http/ContextInternal.h"
#include "http/HttpRequestInternal.h"
#include "http/HttpResponseBodyAccess.h"
#include "runtime/AsioAwait.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/MiddlewareRuntime.h"
#include "ruvia/memory/MemoryPool.h"
#include "ruvia/router/Router.h"
#include "router/RouterInternal.h"
#include "router/RouteResolution.h"
#include "router/RouteTable.h"

namespace {

using ruvia::HttpMethod;
using ruvia::detail::ControllerMiddlewareDescriptor;
using ruvia::detail::RouteHandler;
using ruvia::detail::RouteMatch;
using ruvia::detail::RequestBodyMode;

// Never invoked — resolve() only needs a registered route with a valid handler.
ruvia::Task<ruvia::HttpResponse> dummyHandler(void*, ruvia::Context&) {
    co_return ruvia::HttpResponse(std::pmr::get_default_resource());
}

std::pmr::string path(std::string_view value) {
    return std::pmr::string(value, std::pmr::get_default_resource());
}

void addRoute(ruvia::detail::RouterImpl& impl, HttpMethod method, std::string_view route) {
    impl.registerRoute(
        method,
        path(route),
        RouteHandler(nullptr, &dummyHandler),
        RequestBodyMode::kBuffered,
        std::span<const ControllerMiddlewareDescriptor>{},
        std::span<const ControllerMiddlewareDescriptor>{});
}

void addRoute(ruvia::detail::RouterImpl& impl, std::string_view route) {
    addRoute(impl, HttpMethod::kGet, route);
}

// Registers the given routes and reports whether finalize() rejects them as a
// dynamic route-shape conflict.
bool finalizeConflicts(std::initializer_list<std::string_view> routes) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    for (const auto route : routes) {
        addRoute(impl, route);
    }
    try {
        impl.finalize();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

struct Router final {
    ruvia::Router router;
    ruvia::detail::RouterImpl& impl = ruvia::detail::RouterImpl::from(router);

    void finalize() { impl.finalize(); }

    bool matches(std::string_view p) {
        RouteMatch match;
        return impl.routeTable().resolve(HttpMethod::kGet, p, match).found();
    }

    std::string_view routePathOf(std::string_view p) {
        return routePathOf(HttpMethod::kGet, p);
    }

    std::string_view routePathOf(HttpMethod method, std::string_view p) {
        RouteMatch match;
        const auto res = impl.routeTable().resolve(method, p, match);
        return res.found() ? res.route().path() : "<none>";
    }

    // Returns the single captured param value, or "<none>" if unmatched / no param.
    std::string_view paramOf(std::string_view p) {
        RouteMatch match;
        const auto res = impl.routeTable().resolve(HttpMethod::kGet, p, match);
        if (!res.found() || match.size() != 1) {
            return "<none>";
        }
        return match.values()[0];
    }
};

}  // namespace

RUVIA_TEST(routing_dynamic_exact_match) {
    Router r;
    addRoute(r.impl, "/users/:id");
    r.finalize();
    RUVIA_CHECK(r.matches("/users/42"));
    RUVIA_CHECK_EQ(r.paramOf("/users/42"), std::string_view("42"));
}

RUVIA_TEST(routing_dynamic_trailing_slash_rejected) {
    Router r;
    addRoute(r.impl, "/users/:id");
    r.finalize();
    // Strict matching: a trailing slash is a distinct (empty) segment, so this
    // must NOT collapse to /users/42.
    RUVIA_CHECK(!r.matches("/users/42/"));
}

RUVIA_TEST(routing_dynamic_empty_segment_rejected) {
    Router r;
    addRoute(r.impl, "/a/:x");
    r.finalize();
    RUVIA_CHECK(r.matches("/a/b"));
    RUVIA_CHECK_EQ(r.paramOf("/a/b"), std::string_view("b"));
    // A collapsed // must not silently match and drop the empty segment.
    RUVIA_CHECK(!r.matches("/a//b"));
    RUVIA_CHECK(!r.matches("/a/"));
}

RUVIA_TEST(routing_static_trailing_slash_rejected) {
    Router r;
    addRoute(r.impl, "/users/list");
    addRoute(r.impl, "/users/:id");
    r.finalize();
    RUVIA_CHECK(r.matches("/users/list"));
    RUVIA_CHECK(!r.matches("/users/list/"));
    // /users/list still routes to the static entry (no param), not the dynamic one.
    RUVIA_CHECK_EQ(r.paramOf("/users/list"), std::string_view("<none>"));
    // The dynamic sibling still works.
    RUVIA_CHECK_EQ(r.paramOf("/users/99"), std::string_view("99"));
}

RUVIA_TEST(routing_wildcard_still_matches_tail) {
    Router r;
    addRoute(r.impl, "/files/*");
    r.finalize();
    RUVIA_CHECK(r.matches("/files/a/b/c"));
    RUVIA_CHECK_EQ(r.paramOf("/files/a/b/c"), std::string_view("a/b/c"));
}

RUVIA_TEST(routing_root_wildcard_shadows_param_conflict) {
    // A root wildcard shadows a root param sibling: must error at startup.
    RUVIA_CHECK(finalizeConflicts({"/*", "/:x"}));
    // Deeper-level shadowing already errored; guard it stays that way.
    RUVIA_CHECK(finalizeConflicts({"/a/*", "/a/:x"}));
    // Equivalent param shapes still conflict.
    RUVIA_CHECK(finalizeConflicts({"/users/:id", "/users/:name"}));
}

RUVIA_TEST(routing_root_wildcard_with_static_prefix_allowed) {
    // A root wildcard may coexist with routes distinguished by a static segment.
    RUVIA_CHECK(!finalizeConflicts({"/*", "/users/:id"}));
    RUVIA_CHECK(!finalizeConflicts({"/*", "/health"}));
}

RUVIA_TEST(routing_deep_wildcard_with_static_prefix_allowed) {
    // The static-sibling exemption holds at any depth, not just the root: "public" is a static
    // child tried before the wildcard, so "/files/public/5" -> :id route, "/files/x" -> wildcard,
    // deterministically. (Same situation as the allowed root case /* + /users/:id, one level down.)
    RUVIA_CHECK(!finalizeConflicts({"/files/*", "/files/public/:id"}));
    RUVIA_CHECK(!finalizeConflicts({"/files/*", "/files/list"}));
    RUVIA_CHECK(!finalizeConflicts({"/a/b/*", "/a/b/c/:id"}));
    RUVIA_CHECK(!finalizeConflicts({"/:section/*", "/health/live"}));
    RUVIA_CHECK(!finalizeConflicts({"/files/:bucket/*", "/files/public/:id"}));
    RUVIA_CHECK(!finalizeConflicts({"/:section/*", "/health/*"}));
    RUVIA_CHECK(!finalizeConflicts({"/:section/live", "/health/:probe"}));

    // Guards the fix must NOT regress (these genuinely shadow -> still conflicts):
    RUVIA_CHECK(finalizeConflicts({"/a/*", "/a/:x"}));   // wildcard vs param sibling at a shared node
    RUVIA_CHECK(finalizeConflicts({"/a/*", "/:x/b"}));   // after a static/param fork, the wildcard
                                                         // steals the other route's direct-match path
    RUVIA_CHECK(finalizeConflicts({"/*", "/:x"}));       // root wildcard vs param
    RUVIA_CHECK(finalizeConflicts({"/users/:id", "/users/:name"}));  // two params at one position
}

RUVIA_TEST(routing_allowed_wildcard_overlaps_resolve_to_static_priority_branch) {
    {
        Router r;
        addRoute(r.impl, "/:section/*");
        addRoute(r.impl, "/health/*");
        r.finalize();
        RUVIA_CHECK_EQ(r.routePathOf("/health/live"), std::string_view("/health/*"));
        RUVIA_CHECK_EQ(r.routePathOf("/users/42"), std::string_view("/:section/*"));
    }

    {
        Router r;
        addRoute(r.impl, "/files/:bucket/*");
        addRoute(r.impl, "/files/public/:id");
        r.finalize();
        RUVIA_CHECK_EQ(r.routePathOf("/files/public/5"), std::string_view("/files/public/:id"));
        RUVIA_CHECK_EQ(r.routePathOf("/files/private/a/b"), std::string_view("/files/:bucket/*"));
    }

    {
        Router r;
        addRoute(r.impl, "/:section/live");
        addRoute(r.impl, "/health/:probe");
        r.finalize();
        RUVIA_CHECK_EQ(r.routePathOf("/health/live"), std::string_view("/health/:probe"));
        RUVIA_CHECK_EQ(r.routePathOf("/users/live"), std::string_view("/:section/live"));
    }
}

RUVIA_TEST(routing_head_fallback_respects_static_priority_overlap) {
    Router r;
    addRoute(r.impl, "/:section/live");                   // implicit HEAD fallback
    addRoute(r.impl, HttpMethod::kHead, "/health/:probe"); // explicit HEAD static branch
    r.finalize();

    RUVIA_CHECK_EQ(r.routePathOf(HttpMethod::kHead, "/health/live"), std::string_view("/health/:probe"));
    RUVIA_CHECK_EQ(r.routePathOf(HttpMethod::kHead, "/users/live"), std::string_view("/:section/live"));
}

RUVIA_TEST(routing_explicit_dynamic_head_overrides_exact_get_fallback) {
    Router r;
    addRoute(r.impl, "/health/live");                       // implicit exact HEAD fallback
    addRoute(r.impl, HttpMethod::kHead, "/:section/:probe"); // explicit HEAD route
    r.finalize();

    RUVIA_CHECK_EQ(r.routePathOf(HttpMethod::kHead, "/health/live"), std::string_view("/:section/:probe"));
}

RUVIA_TEST(routing_405_allow_set_lists_the_other_registered_methods) {
    // A request whose method has no route for an existing path is a 405, and the
    // Allow set (RFC 7231 6.5.5) must list exactly the methods that DO have a route
    // for that path -- not methods belonging to other paths, and not the requested
    // method echoed back. This drives the Allow header and was only tested at the
    // RouteResolution value level, never through the route-table computation.
    Router r;
    addRoute(r.impl, HttpMethod::kGet, "/a");
    addRoute(r.impl, HttpMethod::kPost, "/a");
    addRoute(r.impl, HttpMethod::kPut, "/b");
    r.finalize();

    const auto bit = [](HttpMethod m) { return 1U << static_cast<unsigned>(m); };

    RouteMatch match;
    const auto res = r.impl.routeTable().resolve(HttpMethod::kDelete, "/a", match);
    RUVIA_CHECK(!res.found());
    RUVIA_CHECK(res.methodNotAllowed());                 // /a exists for other methods -> 405, not 404
    const auto mask = res.allowedMethods();
    RUVIA_CHECK((mask & bit(HttpMethod::kGet)) != 0);
    RUVIA_CHECK((mask & bit(HttpMethod::kPost)) != 0);
    RUVIA_CHECK((mask & bit(HttpMethod::kHead)) != 0);    // auto-registered alongside the GET route
    RUVIA_CHECK((mask & bit(HttpMethod::kPut)) == 0);     // belongs to /b, not /a
    RUVIA_CHECK((mask & bit(HttpMethod::kDelete)) == 0);  // the requested method is not echoed back

    // A path with no route at all is a 404 (not found), never a 405.
    RouteMatch missMatch;
    const auto missing = r.impl.routeTable().resolve(HttpMethod::kGet, "/nope", missMatch);
    RUVIA_CHECK(!missing.found());
    RUVIA_CHECK(!missing.methodNotAllowed());
}

namespace {

// Records the order middlewares and the handler run: positive on entry (before
// next()), negative on unwind (after next()), 0 for the handler.
std::vector<int> g_chainOrder;

class ChainMwA final : public ruvia::Middleware<ChainMwA> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        g_chainOrder.push_back(1);
        co_await next();
        g_chainOrder.push_back(-1);
    }
};

class ChainMwB final : public ruvia::Middleware<ChainMwB> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        g_chainOrder.push_back(2);
        co_await next();
        g_chainOrder.push_back(-2);
    }
};

// Short-circuits: sets a response and does NOT call next().
class ChainMwStop final : public ruvia::Middleware<ChainMwStop> {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next&) {
        g_chainOrder.push_back(9);
        context.res(context.body("stopped"));
        co_return;
    }
};

ruvia::Task<ruvia::HttpResponse> chainHandler(void*, ruvia::Context& context) {
    g_chainOrder.push_back(0);
    co_return context.body("ok");
}

std::string dispatchChain(std::span<const ControllerMiddlewareDescriptor> middlewares) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(
        HttpMethod::kGet, path("/chain"), RouteHandler(nullptr, &chainHandler),
        RequestBodyMode::kBuffered, middlewares,
        std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, HttpMethod::kGet);
    ruvia::detail::HttpRequestAccess::setPath(request, "/chain");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})),
        asio::use_future);
    ctx.run();
    auto response = future.get();
    const auto body = ruvia::detail::responseBodyBytes(response);
    return std::string(body.data(), body.size());
}

}  // namespace

RUVIA_TEST(middleware_chain_runs_in_onion_order) {
    g_chainOrder.clear();
    const ControllerMiddlewareDescriptor mws[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwA>(),
        ruvia::detail::makeMiddlewareDescriptor<ChainMwB>(),
    };
    const auto body = dispatchChain(std::span<const ControllerMiddlewareDescriptor>(mws, 2));
    RUVIA_CHECK_EQ(body, std::string("ok"));
    // Onion order: A pre, B pre, handler, B post, A post.
    const std::vector<int> expected{1, 2, 0, -2, -1};
    RUVIA_CHECK(g_chainOrder == expected);
}

RUVIA_TEST(middleware_chain_short_circuits_without_next) {
    g_chainOrder.clear();
    // A middleware that does not call next() stops the chain: the next middleware
    // and the handler never run, and the middleware's response is used.
    const ControllerMiddlewareDescriptor mws[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwStop>(),
        ruvia::detail::makeMiddlewareDescriptor<ChainMwA>(),
    };
    const auto body = dispatchChain(std::span<const ControllerMiddlewareDescriptor>(mws, 2));
    RUVIA_CHECK_EQ(body, std::string("stopped"));
    const std::vector<int> expected{9};
    RUVIA_CHECK(g_chainOrder == expected);
}

namespace {

ruvia::Task<ruvia::HttpResponse> throwsHttpErrorHandler(void*, ruvia::Context&) {
    throw ruvia::HttpError(403, "forbidden", "nope");
    co_return ruvia::HttpResponse(std::pmr::get_default_resource());  // unreachable
}

ruvia::Task<ruvia::HttpResponse> throwsGenericHandler(void*, ruvia::Context&) {
    throw std::runtime_error("boom");
    co_return ruvia::HttpResponse(std::pmr::get_default_resource());  // unreachable
}

ruvia::Task<ruvia::HttpResponse> okHandler(void*, ruvia::Context& context) {
    co_return context.body("ok");
}

// The dispatched response's storage lives in the per-request arena, which is
// destroyed when the helper returns -- so values must be copied out here, while
// the arena is still alive, rather than returning the HttpResponse itself.
struct DispatchResult final {
    std::uint16_t status{0};
    std::string body;
    std::string allow;
};

DispatchResult extractDispatchResult(const ruvia::HttpResponse& response) {
    DispatchResult result;
    result.status = response.status();
    const auto body = ruvia::detail::responseBodyBytes(response);
    result.body.assign(body.data(), body.size());
    const auto allow = response.header("Allow");
    result.allow.assign(allow.data(), allow.size());
    return result;
}

// Registers GET /x with `handler`, dispatches `method path`, returns the result.
DispatchResult dispatchOne(RouteHandler handler, HttpMethod method, std::string_view p) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(HttpMethod::kGet, path("/x"), handler, RequestBodyMode::kBuffered,
                       std::span<const ControllerMiddlewareDescriptor>{},
                       std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, method);
    ruvia::detail::HttpRequestAccess::setPath(request, p);
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})),
        asio::use_future);
    ctx.run();
    return extractDispatchResult(future.get());  // arena still alive here
}

}  // namespace

RUVIA_TEST(dispatch_maps_handler_exceptions_to_error_responses) {
    // A thrown HttpError surfaces with its own status; any other exception is a 500.
    RUVIA_CHECK_EQ(dispatchOne(RouteHandler(nullptr, &throwsHttpErrorHandler), HttpMethod::kGet, "/x").status,
                   std::uint16_t{403});
    RUVIA_CHECK_EQ(dispatchOne(RouteHandler(nullptr, &throwsGenericHandler), HttpMethod::kGet, "/x").status,
                   std::uint16_t{500});
}

RUVIA_TEST(dispatch_produces_404_and_405_for_unmatched_routes) {
    // A path with no route -> 404.
    RUVIA_CHECK_EQ(dispatchOne(RouteHandler(nullptr, &okHandler), HttpMethod::kGet, "/nope").status,
                   std::uint16_t{404});
    // The path exists but the method does not -> 405 with an Allow header listing GET.
    const auto notAllowed = dispatchOne(RouteHandler(nullptr, &okHandler), HttpMethod::kPost, "/x");
    RUVIA_CHECK_EQ(notAllowed.status, std::uint16_t{405});
    RUVIA_CHECK(notAllowed.allow.find("GET") != std::string_view::npos);
    // The registered method still works.
    RUVIA_CHECK_EQ(dispatchOne(RouteHandler(nullptr, &okHandler), HttpMethod::kGet, "/x").status,
                   std::uint16_t{200});
}

namespace {

using ruvia::HttpErrorHandler;
using ruvia::HttpErrorInfo;
using ruvia::HttpNotFoundHandler;

ruvia::Task<ruvia::HttpResponse> customNotFound(ruvia::Context& context) {
    co_return context.body("custom-not-found", ruvia::Context::ResponseInit{.status = 404});
}

ruvia::Task<ruvia::HttpResponse> customError(ruvia::Context& context, HttpErrorInfo info) {
    co_return context.body("custom-error", ruvia::Context::ResponseInit{.status = info.status()});
}

DispatchResult dispatchWithHandlers(
    RouteHandler handler, HttpErrorHandler errorH, HttpNotFoundHandler notFoundH,
    HttpMethod method, std::string_view p) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    if (errorH != nullptr) {
        impl.setErrorHandler(errorH);
    }
    if (notFoundH != nullptr) {
        impl.setNotFoundHandler(notFoundH);
    }
    impl.registerRoute(HttpMethod::kGet, path("/x"), handler, RequestBodyMode::kBuffered,
                       std::span<const ControllerMiddlewareDescriptor>{},
                       std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, method);
    ruvia::detail::HttpRequestAccess::setPath(request, p);
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})),
        asio::use_future);
    ctx.run();
    return extractDispatchResult(future.get());  // arena still alive here
}

}  // namespace

RUVIA_TEST(dispatch_uses_custom_not_found_handler) {
    // A registered not-found handler replaces the default 404 response body.
    const auto result = dispatchWithHandlers(
        RouteHandler(nullptr, &okHandler), nullptr, &customNotFound, HttpMethod::kGet, "/nope");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{404});
    RUVIA_CHECK_EQ(result.body, std::string("custom-not-found"));
}

RUVIA_TEST(dispatch_uses_custom_error_handler_with_thrown_status) {
    // A registered error handler renders a thrown HttpError, preserving its status.
    const auto result = dispatchWithHandlers(
        RouteHandler(nullptr, &throwsHttpErrorHandler), &customError, nullptr, HttpMethod::kGet, "/x");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{403});
    RUVIA_CHECK_EQ(result.body, std::string("custom-error"));
}

RUVIA_TEST(dispatch_options_asterisk_returns_server_wide_allow) {
    // A server-wide OPTIONS * request is answered with 204 and an Allow header
    // listing every method registered anywhere on the server, not per-route.
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(HttpMethod::kGet, path("/a"), RouteHandler(nullptr, &okHandler),
                       RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{},
                       std::span<const ControllerMiddlewareDescriptor>{});
    impl.registerRoute(HttpMethod::kPost, path("/b"), RouteHandler(nullptr, &okHandler),
                       RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{},
                       std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, HttpMethod::kOptions);
    ruvia::detail::HttpRequestAccess::setPath(request, "*");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})),
        asio::use_future);
    ctx.run();
    const auto response = future.get();

    RUVIA_CHECK_EQ(response.status(), std::uint16_t{204});
    const auto allow = response.header("Allow");
    RUVIA_CHECK(allow.find("GET") != std::string_view::npos);
    RUVIA_CHECK(allow.find("POST") != std::string_view::npos);
}
