#include "routing_fixture.h"
#include "ruvia/web/BodyLimit.h"
#include "ruvia/web/Deadline.h"

// Routing: registering routes and matching a request to one.

using TestRouteRateLimit = ruvia::RateLimit<1, 1000>;

RUVIA_TEST(compiled_route_plan_is_shared_across_worker_bindings) {
    ruvia::detail::Router firstRouter;
    auto& first = ruvia::detail::RouterImpl::from(firstRouter);
    addRoute(first, "/health");
    addRoute(first, "/users/:id");
    first.finalize();
    auto plan = first.releaseCompiledPlan();

    ruvia::detail::Router secondRouter;
    auto& second = ruvia::detail::RouterImpl::from(secondRouter);
    addRoute(second, "/health");
    addRoute(second, "/users/:id");
    second.finalize(plan.get());

    const auto staticResolution = second.routeTable().resolve(HttpKnownMethod::kGet, "/health");
    const auto dynamicResolution = second.routeTable().resolve(HttpKnownMethod::kGet, "/users/42");
    RUVIA_CHECK(staticResolution.resolved() != nullptr);
    RUVIA_CHECK(dynamicResolution.resolved() != nullptr);
    RUVIA_CHECK_EQ(dynamicResolution.resolved()->match().size(), std::size_t{1});
    RUVIA_CHECK_EQ(dynamicResolution.resolved()->match().values()[0], std::string_view("42"));
}

RUVIA_TEST(compiled_route_plan_rejects_a_different_worker_route_shape) {
    ruvia::detail::Router firstRouter;
    auto& first = ruvia::detail::RouterImpl::from(firstRouter);
    addRoute(first, "/expected");
    first.finalize();
    auto plan = first.releaseCompiledPlan();

    ruvia::detail::Router differentRouter;
    auto& different = ruvia::detail::RouterImpl::from(differentRouter);
    addRoute(different, "/different");
    bool rejected = false;
    try {
        different.finalize(plan.get());
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(compiled_route_plan_rejects_a_different_worker_endpoint_contract) {
    ruvia::detail::Router firstRouter;
    auto& first = ruvia::detail::RouterImpl::from(firstRouter);
    addRoute(first, "/events");
    first.finalize();
    auto plan = first.releaseCompiledPlan();

    ruvia::detail::Router differentRouter;
    auto& different = ruvia::detail::RouterImpl::from(differentRouter);
    different.registerResponseStreamRoute(HttpKnownMethod::kGet, path("/events"),
        ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler),
        std::span<const ControllerMiddlewareDescriptor>{},
        std::span<const ControllerMiddlewareDescriptor>{});
    bool rejected = false;
    try {
        different.finalize(plan.get());
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(route_rejects_duplicate_validated_model_types_at_registration) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    const auto controllerValidator = ruvia::detail::makeMiddlewareDescriptor<FirstIntValidator>();
    const auto routeValidator = ruvia::detail::makeMiddlewareDescriptor<SecondIntValidator>();

    bool rejected = false;
    try {
        impl.registerRoute(HttpKnownMethod::kPost, path("/duplicate-validated-model"),
            RouteHandler(nullptr, &dummyHandler), RequestBodyMode::kBuffered,
            std::span(&controllerValidator, std::size_t{1}),
            std::span(&routeValidator, std::size_t{1}));
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) == "duplicate validated model type on route";
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(finalized_route_table_records_route_rate_limit_usage) {
    {
        ruvia::detail::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        addRoute(impl, "/plain");
        impl.finalize();
        RUVIA_CHECK(!impl.routeTable().hasRouteRateLimit());
    }

    {
        ruvia::detail::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        const auto rateLimit = ruvia::detail::makeMiddlewareDescriptor<TestRouteRateLimit>();
        impl.registerRoute(HttpKnownMethod::kGet, path("/limited"),
            RouteHandler(nullptr, &dummyHandler), RequestBodyMode::kBuffered,
            std::span<const ControllerMiddlewareDescriptor>{},
            std::span(&rateLimit, std::size_t{1}));
        impl.finalize();
        RUVIA_CHECK(impl.routeTable().hasRouteRateLimit());
    }
}

RUVIA_TEST(finalized_route_table_carries_the_declared_body_limit) {
    using SmallBody = ruvia::BodyLimit<16>;
    using SmallerBody = ruvia::BodyLimit<8>;

    {
        // Undeclared stays 0, meaning "use the server's limit".
        ruvia::detail::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        addRoute(impl, "/plain");
        impl.finalize();
        const auto resolution = impl.routeTable().resolve(HttpKnownMethod::kGet, "/plain");
        RUVIA_CHECK(resolution.resolved() != nullptr);
        RUVIA_CHECK_EQ(resolution.resolved()->route().maxRequestBodyBytes(), std::size_t{0});
    }

    {
        ruvia::detail::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        const auto limit = ruvia::detail::makeMiddlewareDescriptor<SmallBody>();
        impl.registerRoute(HttpKnownMethod::kGet, path("/limited"),
            RouteHandler(nullptr, &dummyHandler), RequestBodyMode::kBuffered,
            std::span<const ControllerMiddlewareDescriptor>{}, std::span(&limit, std::size_t{1}));
        impl.finalize();
        const auto resolution = impl.routeTable().resolve(HttpKnownMethod::kGet, "/limited");
        RUVIA_CHECK(resolution.resolved() != nullptr);
        RUVIA_CHECK_EQ(resolution.resolved()->route().maxRequestBodyBytes(), std::size_t{16});
    }

    {
        // A controller-wide and a route-specific declaration: the stricter wins,
        // regardless of which position it sits in.
        ruvia::detail::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        const auto controllerLimit = ruvia::detail::makeMiddlewareDescriptor<SmallBody>();
        const auto routeLimit = ruvia::detail::makeMiddlewareDescriptor<SmallerBody>();
        impl.registerRoute(HttpKnownMethod::kGet, path("/both"),
            RouteHandler(nullptr, &dummyHandler), RequestBodyMode::kBuffered,
            std::span(&controllerLimit, std::size_t{1}), std::span(&routeLimit, std::size_t{1}));
        impl.finalize();
        const auto resolution = impl.routeTable().resolve(HttpKnownMethod::kGet, "/both");
        RUVIA_CHECK(resolution.resolved() != nullptr);
        RUVIA_CHECK_EQ(resolution.resolved()->route().maxRequestBodyBytes(), std::size_t{8});
    }
}

RUVIA_TEST(finalized_route_table_carries_the_declared_deadline) {
    using SlowRoute = ruvia::Deadline<30000>;
    using FastRoute = ruvia::Deadline<500>;

    {
        ruvia::detail::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        addRoute(impl, "/plain");
        impl.finalize();
        const auto resolution = impl.routeTable().resolve(HttpKnownMethod::kGet, "/plain");
        RUVIA_CHECK(resolution.resolved() != nullptr);
        RUVIA_CHECK_EQ(resolution.resolved()->route().deadlineMs(), std::int64_t{0});
    }

    {
        // Controller-wide and route-specific: the stricter wins, not the nearer.
        // A route asking for MORE time than its controller declared does not get
        // it -- the one rule every app/route policy follows.
        ruvia::detail::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        const auto controllerDeadline = ruvia::detail::makeMiddlewareDescriptor<FastRoute>();
        const auto routeDeadline = ruvia::detail::makeMiddlewareDescriptor<SlowRoute>();
        impl.registerRoute(HttpKnownMethod::kGet, path("/both"),
            RouteHandler(nullptr, &dummyHandler), RequestBodyMode::kBuffered,
            std::span(&controllerDeadline, std::size_t{1}),
            std::span(&routeDeadline, std::size_t{1}));
        impl.finalize();
        const auto resolution = impl.routeTable().resolve(HttpKnownMethod::kGet, "/both");
        RUVIA_CHECK(resolution.resolved() != nullptr);
        RUVIA_CHECK_EQ(resolution.resolved()->route().deadlineMs(), std::int64_t{500});
    }
}

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
    RUVIA_CHECK(
        finalizeConflicts({"/a/*", "/a/:x"}));          // wildcard vs param sibling at a shared node
    RUVIA_CHECK(finalizeConflicts({"/a/*", "/:x/b"}));  // after a static/param fork, the wildcard
        // steals the other route's direct-match path
    RUVIA_CHECK(finalizeConflicts({"/*", "/:x"}));                   // root wildcard vs param
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
    addRoute(r.impl, "/:section/live");                          // implicit HEAD fallback
    addRoute(r.impl, HttpKnownMethod::kHead, "/health/:probe");  // explicit HEAD static branch
    r.finalize();

    RUVIA_CHECK_EQ(
        r.routePathOf(HttpKnownMethod::kHead, "/health/live"), std::string_view("/health/:probe"));
    RUVIA_CHECK_EQ(
        r.routePathOf(HttpKnownMethod::kHead, "/users/live"), std::string_view("/:section/live"));
}

RUVIA_TEST(routing_explicit_dynamic_head_overrides_exact_get_fallback) {
    Router r;
    addRoute(r.impl, "/health/live");                              // implicit exact HEAD fallback
    addRoute(r.impl, HttpKnownMethod::kHead, "/:section/:probe");  // explicit HEAD route
    r.finalize();

    RUVIA_CHECK_EQ(r.routePathOf(HttpKnownMethod::kHead, "/health/live"),
        std::string_view("/:section/:probe"));
}

RUVIA_TEST(routing_405_allow_set_lists_the_other_registered_methods) {
    // A request whose method has no route for an existing path is a 405, and the
    // Allow set (RFC 7231 6.5.5) must list exactly the methods that DO have a route
    // for that path -- not methods belonging to other paths, and not the requested
    // method echoed back. This drives the Allow header and was only tested at the
    // RouteResolution value level, never through the route-table computation.
    Router r;
    addRoute(r.impl, HttpKnownMethod::kGet, "/a");
    addRoute(r.impl, HttpKnownMethod::kPost, "/a");
    addRoute(r.impl, HttpKnownMethod::kPut, "/b");
    r.finalize();

    const auto bit = [](HttpKnownMethod m) { return 1U << static_cast<unsigned>(m); };

    const auto res = r.impl.routeTable().resolve(HttpKnownMethod::kDelete, "/a");
    RUVIA_CHECK(res.resolved() == nullptr);
    const auto* methodNotAllowed = res.methodNotAllowed();
    RUVIA_CHECK(methodNotAllowed != nullptr);  // /a exists for other methods -> 405
    const auto mask = methodNotAllowed->allowedMethods();
    RUVIA_CHECK((mask & bit(HttpKnownMethod::kGet)) != 0);
    RUVIA_CHECK((mask & bit(HttpKnownMethod::kPost)) != 0);
    RUVIA_CHECK(
        (mask & bit(HttpKnownMethod::kHead)) != 0);         // auto-registered alongside the GET route
    RUVIA_CHECK((mask & bit(HttpKnownMethod::kPut)) == 0);  // belongs to /b, not /a
    RUVIA_CHECK(
        (mask & bit(HttpKnownMethod::kDelete)) == 0);  // the requested method is not echoed back

    // A path with no route at all is a 404 (not found), never a 405.
    const auto missing = r.impl.routeTable().resolve(HttpKnownMethod::kGet, "/nope");
    RUVIA_CHECK(missing.resolved() == nullptr);
    RUVIA_CHECK(missing.methodNotAllowed() == nullptr);
    RUVIA_CHECK(missing.notFound() != nullptr);
}

RUVIA_TEST(routing_options_only_resource_is_405_not_404) {
    // A path whose only registered method is OPTIONS must answer 405 (method known
    // but unsupported, RFC 9110 15.5.6), listing OPTIONS in Allow -- not 404, since
    // the resource exists. allowedMethods used to clear the OPTIONS bit and so
    // returned an empty set, making resolve() report not-found.
    Router r;
    addRoute(r.impl, HttpKnownMethod::kOptions, "/preflight");
    r.finalize();
    const auto bit = [](HttpKnownMethod m) { return 1U << static_cast<unsigned>(m); };

    const auto res = r.impl.routeTable().resolve(HttpKnownMethod::kGet, "/preflight");
    RUVIA_CHECK(res.resolved() == nullptr);
    RUVIA_CHECK(res.methodNotAllowed() != nullptr);  // 405, not 404
    RUVIA_CHECK((res.methodNotAllowed()->allowedMethods() & bit(HttpKnownMethod::kOptions)) != 0);

    // The explicit OPTIONS route still handles an OPTIONS request to that path.
    const auto preflight = r.impl.routeTable().resolve(HttpKnownMethod::kOptions, "/preflight");
    RUVIA_CHECK(preflight.resolved() != nullptr);
}

RUVIA_TEST(routing_options_asterisk_not_captured_by_wildcard_route) {
    // RFC 9110 7.1 / 9.3.7: "OPTIONS *" is a server-wide request, not a resource one.
    // A catch-all OPTIONS route must NOT capture it (the wildcard node otherwise
    // would), so it stays unresolved and dispatch emits the server-wide response.
    Router r;
    addRoute(r.impl, HttpKnownMethod::kOptions, "/*");
    addRoute(r.impl, HttpKnownMethod::kGet, "/*");
    r.finalize();

    const auto asterisk = r.impl.routeTable().resolve(HttpKnownMethod::kOptions, "*");
    RUVIA_CHECK(asterisk.notFound() != nullptr);

    // A normal path still matches the catch-all: the short-circuit is only for "*".
    const auto wildcard = r.impl.routeTable().resolve(HttpKnownMethod::kOptions, "/anything");
    RUVIA_CHECK(wildcard.resolved() != nullptr);
}

RUVIA_TEST(routing_rejects_duplicate_route_registration) {
    Router r;
    addRoute(r.impl, HttpKnownMethod::kGet, "/x");
    // The same method+path registered twice is a duplicate: ambiguous routing is
    // rejected at registration rather than one route silently shadowing the other.
    bool threw = false;
    try {
        addRoute(r.impl, HttpKnownMethod::kGet, "/x");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    // The SAME path under a DIFFERENT method is not a duplicate.
    addRoute(r.impl, HttpKnownMethod::kPost, "/x");
    r.finalize();
    RUVIA_CHECK(r.matches("/x"));
}

RUVIA_TEST(routing_rejects_invalid_route_paths_at_registration) {
    const auto rejects = [](std::string_view route) {
        ruvia::detail::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        try {
            addRoute(impl, HttpKnownMethod::kGet, route);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    RUVIA_CHECK(rejects(""));
    RUVIA_CHECK(rejects("relative"));
    RUVIA_CHECK(rejects("*"));
    RUVIA_CHECK(rejects("/x?debug=1"));
    RUVIA_CHECK(rejects("/bad path"));
    RUVIA_CHECK(rejects("/x#fragment"));
}

RUVIA_TEST(routing_rejects_registration_after_finalize) {
    Router r;
    addRoute(r.impl, HttpKnownMethod::kGet, "/x");
    r.finalize();
    // The route table is immutable once finalized; a late registration must throw
    // rather than mutate the already-built table.
    bool threw = false;
    try {
        addRoute(r.impl, HttpKnownMethod::kGet, "/y");
    } catch (const std::logic_error&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(url_for_builds_paths_from_registered_patterns) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    for (const auto route : {"/users/:id", "/files/*", "/about", "/a/:x/b/:y"}) {
        impl.registerRoute(HttpKnownMethod::kGet, path(route), RouteHandler(nullptr, &okHandler),
            RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{},
            std::span<const ControllerMiddlewareDescriptor>{});
    }
    impl.finalize();
    const auto& table = impl.routeTable();
    auto* resource = std::pmr::get_default_resource();

    const auto urlFor = [&](std::string_view pattern,
                            std::initializer_list<std::string_view> values) {
        return std::string(table.urlFor(
            pattern, std::span<const std::string_view>(values.begin(), values.size()), resource));
    };

    RUVIA_CHECK_EQ(urlFor("/users/:id", {"42"}), std::string("/users/42"));
    // Parameter values are percent-encoded as one path segment.
    RUVIA_CHECK_EQ(urlFor("/users/:id", {"a b/c"}), std::string("/users/a%20b%2Fc"));
    // A wildcard value keeps its slashes; other bytes are still encoded.
    RUVIA_CHECK_EQ(urlFor("/files/*", {"x/y z"}), std::string("/files/x/y%20z"));
    // An empty wildcard capture addresses the bare mount path.
    RUVIA_CHECK_EQ(urlFor("/files/*", {""}), std::string("/files"));
    RUVIA_CHECK_EQ(urlFor("/about", {}), std::string("/about"));
    RUVIA_CHECK_EQ(urlFor("/a/:x/b/:y", {"1", "2"}), std::string("/a/1/b/2"));

    const auto throws = [&](std::string_view pattern,
                            std::initializer_list<std::string_view> values) {
        try {
            (void)urlFor(pattern, values);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    // The pattern is the route's identity: unregistered patterns are refused.
    RUVIA_CHECK(throws("/users/:name", {"42"}));
    RUVIA_CHECK(throws("/users/:id", {}));
    RUVIA_CHECK(throws("/about", {"extra"}));
    // A dynamic segment never matches empty, so building one is refused too.
    RUVIA_CHECK(throws("/users/:id", {""}));
}

RUVIA_TEST(context_url_for_uses_dispatch_bound_route_table) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(HttpKnownMethod::kGet, path("/echo"),
        RouteHandler(nullptr, &urlForEchoHandler), RequestBodyMode::kBuffered,
        std::span<const ControllerMiddlewareDescriptor>{},
        std::span<const ControllerMiddlewareDescriptor>{});
    impl.registerRoute(HttpKnownMethod::kGet, path("/users/:id"), RouteHandler(nullptr, &okHandler),
        RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{},
        std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    const auto& table = impl.routeTable();

    RUVIA_CHECK_EQ(dispatchOn(table, "GET", "/echo").body, std::string("/users/7"));
}
