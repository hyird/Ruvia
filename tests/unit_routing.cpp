#include "test_harness.h"

#include <exception>
#include <memory_resource>
#include <span>
#include <string_view>

#include "ruvia/http/Context.h"
#include "ruvia/router/Router.h"
#include "router/RouterInternal.h"
#include "router/RouteResolution.h"
#include "router/RouteTable.h"

namespace {

using ruvia::HttpMethod;
using ruvia::detail::ControllerMiddlewareDescriptor;
using ruvia::detail::RouteHandler;
using ruvia::detail::RouteMatch;
using ruvia::RequestBodyMode;

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
