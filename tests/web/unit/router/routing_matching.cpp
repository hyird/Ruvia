#include <array>
#include <cstdint>
#include <string>

#include "ruvia/web/body_limit.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/deadline.h"

#include "routing_fixture.h"

// Routing: registering routes and matching a request to one.

using test_route_rate_limit_type = ruvia::rate_limit<1, 1000>;

struct replay_safe_contract_middleware final : ruvia::middleware {
    static constexpr bool ruvia_replay_safe = true;
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
    }
};

struct replay_unsafe_contract_middleware final : ruvia::middleware {
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
    }
};

RUVIA_TEST(route_lists_preserve_all_entries) {
    const ruvia::detail::ruvia_path_list paths("/0", "/1", "/2", "/3", "/4", "/5", "/6", "/7", "/8", "/9");
    RUVIA_CHECK_EQ(paths.end() - paths.begin(), 10);
    RUVIA_CHECK_EQ(paths.begin()[9], std::string_view("/9"));
    const ruvia::detail::ruvia_method_list methods(http_known_method::get, http_known_method::post);
    RUVIA_CHECK_EQ(methods.end() - methods.begin(), 2);
    RUVIA_CHECK(methods.begin()[1] == http_known_method::post);
}

RUVIA_TEST(compiled_route_plan_is_shared_across_worker_bindings) {
    ruvia::detail::router first_router;
    auto& first = ruvia::detail::router_impl::from(first_router);
    add_route(first, "/health");
    add_route(first, "/users/:id");
    first.finalize();
    auto plan = first.release_compiled_plan();

    ruvia::detail::router second_router;
    auto& second = ruvia::detail::router_impl::from(second_router);
    add_route(second, "/health");
    add_route(second, "/users/:id");
    second.finalize(plan.get());

    const auto static_resolution = second.route_table().resolve(http_known_method::get, "/health");
    const auto dynamic_resolution = second.route_table().resolve(http_known_method::get, "/users/42");
    RUVIA_CHECK(static_resolution.resolved() != nullptr);
    RUVIA_CHECK(dynamic_resolution.resolved() != nullptr);
    RUVIA_CHECK_EQ(dynamic_resolution.resolved()->match().size(), std::size_t{1});
    RUVIA_CHECK_EQ(dynamic_resolution.resolved()->match().values()[0], std::string_view("42"));
}

RUVIA_TEST(routing_static_paths_preserve_method_and_path_identity) {
    router router;
    std::array<std::string, 257> paths;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        paths[i] = "/api/resource/" + std::to_string(i) + "-" + std::to_string(i * 7919) + "/details";
        add_route(router.impl_, paths[i]);
        if (i % 2 == 0) {
            add_route(router.impl_, http_known_method::post, paths[i]);
        }
        if (i % 3 == 0) {
            add_route(router.impl_, http_known_method::head, paths[i]);
        }
    }
    router.finalize();

    const auto method_bit = [](http_known_method method) {
        return std::uint32_t{1} << static_cast<unsigned>(method);
    };
    const auto& table_value = router.impl_.route_table();
    for (std::size_t i = 0; i < paths.size(); ++i) {
        for (const auto method : {http_known_method::get, http_known_method::head}) {
            const auto result_value = table_value.resolve(method, paths[i]);
            RUVIA_CHECK(result_value.resolved() != nullptr);
            RUVIA_CHECK_EQ(result_value.resolved()->route().path(), std::string_view(paths[i]));
            const auto registered_method = method == http_known_method::head && i % 3 != 0
                                               ? http_known_method::get
                                               : method;
            RUVIA_CHECK(result_value.resolved()->route().method() == registered_method);
        }
        const auto post = table_value.resolve(http_known_method::post, paths[i]);
        if (i % 2 == 0) {
            RUVIA_CHECK(post.resolved() != nullptr);
            RUVIA_CHECK(post.resolved()->route().method() == http_known_method::post);
            RUVIA_CHECK_EQ(post.resolved()->route().path(), std::string_view(paths[i]));
        } else {
            RUVIA_CHECK(post.method_not_allowed() != nullptr);
        }
        const auto unsupported = table_value.resolve(http_known_method::put, paths[i]);
        RUVIA_CHECK(unsupported.method_not_allowed() != nullptr);
        const auto allowed = method_bit(http_known_method::get) | method_bit(http_known_method::head) |
                             method_bit(http_known_method::options) |
                             (i % 2 == 0 ? method_bit(http_known_method::post) : 0);
        RUVIA_CHECK_EQ(unsupported.method_not_allowed()->allowed_methods(), allowed);
        const auto missing = table_value.resolve(http_known_method::get, paths[i] + "/missing");
        RUVIA_CHECK(missing.not_found() != nullptr);
    }
}

RUVIA_TEST(compiled_route_plan_outlives_its_first_worker_binding) {
    std::array<std::string, 65> paths;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        paths[i] = "/api/resource/" + std::to_string(i) + "/details";
    }
    auto plan = [&] {
        router original;
        for (const auto& route_path : paths) {
            add_route(original.impl_, route_path);
        }
        original.finalize();
        return original.impl_.release_compiled_plan();
    }();
    router rebound;
    for (const auto& route_path : paths) {
        add_route(rebound.impl_, route_path);
    }
    rebound.impl_.finalize(plan.get());
    for (const auto& route_path : paths) {
        const auto result_value = rebound.impl_.route_table().resolve(http_known_method::get, route_path);
        RUVIA_CHECK(result_value.resolved() != nullptr);
        RUVIA_CHECK_EQ(result_value.resolved()->route().path(), std::string_view(route_path));
    }
}

RUVIA_TEST(compiled_route_plan_binds_replay_safe_route_contract) {
    const auto safe_middleware =
        ruvia::detail::make_middleware_descriptor<replay_safe_contract_middleware>();
    const auto unsafe_middleware =
        ruvia::detail::make_middleware_descriptor<replay_unsafe_contract_middleware>();
    const std::array mixed_middlewares{safe_middleware, unsafe_middleware};
    ruvia::detail::router first_router;
    auto& first = ruvia::detail::router_impl::from(first_router);
    first.register_route(http_known_method::get, path("/safe"),
        route_handler_type(nullptr, &dummy_handler), request_body_mode::buffered, {},
        std::span(&safe_middleware, std::size_t{1}));
    first.register_route(http_known_method::get, path("/default"),
        route_handler_type(nullptr, &dummy_handler), request_body_mode::buffered, {}, {});
    first.register_route(http_known_method::get, path("/mixed"),
        route_handler_type(nullptr, &dummy_handler), request_body_mode::buffered, {},
        std::span(mixed_middlewares));
    first.finalize();
    const auto safe = first.route_table().resolve(http_known_method::get, "/safe");
    const auto ordinary = first.route_table().resolve(http_known_method::get, "/default");
    const auto mixed = first.route_table().resolve(http_known_method::get, "/mixed");
    RUVIA_CHECK(safe.resolved()->route().endpoint().buffered()->replay_safe());
    RUVIA_CHECK(!ordinary.resolved()->route().endpoint().buffered()->replay_safe());
    RUVIA_CHECK(!mixed.resolved()->route().endpoint().buffered()->replay_safe());
    auto plan = first.release_compiled_plan();

    ruvia::detail::router different_router;
    auto& different = ruvia::detail::router_impl::from(different_router);
    different.register_route(http_known_method::get, path("/safe"),
        route_handler_type(nullptr, &dummy_handler), request_body_mode::buffered, {}, {});
    bool rejected = false;
    try {
        different.finalize(plan.get());
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(compiled_route_plan_rejects_a_different_worker_route_shape) {
    ruvia::detail::router first_router;
    auto& first = ruvia::detail::router_impl::from(first_router);
    add_route(first, "/expected");
    first.finalize();
    auto plan = first.release_compiled_plan();

    ruvia::detail::router different_router;
    auto& different = ruvia::detail::router_impl::from(different_router);
    add_route(different, "/different");
    bool rejected = false;
    try {
        different.finalize(plan.get());
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    different.finalize();
    const auto recovered = different.route_table().resolve(http_known_method::get, "/different");
    RUVIA_CHECK(recovered.resolved() != nullptr);
}

RUVIA_TEST(compiled_route_plan_rejects_a_different_worker_endpoint_contract) {
    ruvia::detail::router first_router;
    auto& first = ruvia::detail::router_impl::from(first_router);
    add_route(first, "/events");
    first.finalize();
    auto plan = first.release_compiled_plan();

    ruvia::detail::router different_router;
    auto& different = ruvia::detail::router_impl::from(different_router);
    different.register_response_stream_route(http_known_method::get, path("/events"),
        ruvia::detail::route_stream_handler_type(nullptr, &dummy_stream_handler),
        std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
    bool rejected = false;
    try {
        different.finalize(plan.get());
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(route_rejects_duplicate_validated_model_types_at_registration) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    const auto controller_validator = ruvia::detail::make_middleware_descriptor<first_int_validator>();
    const auto route_validator = ruvia::detail::make_middleware_descriptor<second_int_validator>();

    bool rejected = false;
    try {
        impl.register_route(http_known_method::post, path("/duplicate-validated-model"),
            route_handler_type(nullptr, &dummy_handler), request_body_mode::buffered,
            std::span(&controller_validator, std::size_t{1}),
            std::span(&route_validator, std::size_t{1}));
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) == "duplicate validated model type on route";
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(finalized_route_table_records_route_rate_limit_usage) {
    {
        ruvia::detail::router router;
        auto& impl = ruvia::detail::router_impl::from(router);
        add_route(impl, "/plain");
        impl.finalize();
        RUVIA_CHECK(!impl.route_table().has_route_rate_limit());
    }

    {
        ruvia::detail::router router;
        auto& impl = ruvia::detail::router_impl::from(router);
        const auto rate_limit_value = ruvia::detail::make_middleware_descriptor<test_route_rate_limit_type>();
        impl.register_route(http_known_method::get, path("/limited"),
            route_handler_type(nullptr, &dummy_handler), request_body_mode::buffered,
            std::span<const controller_middleware_descriptor>{},
            std::span(&rate_limit_value, std::size_t{1}));
        impl.finalize();
        RUVIA_CHECK(impl.route_table().has_route_rate_limit());
    }
}

RUVIA_TEST(finalized_route_table_carries_the_declared_body_limit) {
    using small_body_type = ruvia::body_limit<16>;
    using smaller_body_type = ruvia::body_limit<8>;

    {
        // Undeclared stays 0, meaning "use the server's limit".
        ruvia::detail::router router;
        auto& impl = ruvia::detail::router_impl::from(router);
        add_route(impl, "/plain");
        impl.finalize();
        const auto resolution = impl.route_table().resolve(http_known_method::get, "/plain");
        RUVIA_CHECK(resolution.resolved() != nullptr);
        RUVIA_CHECK_EQ(resolution.resolved()->route().max_request_body_bytes(), std::size_t{0});
    }

    {
        ruvia::detail::router router;
        auto& impl = ruvia::detail::router_impl::from(router);
        const auto limit = ruvia::detail::make_middleware_descriptor<small_body_type>();
        impl.register_route(http_known_method::get, path("/limited"),
            route_handler_type(nullptr, &dummy_handler), request_body_mode::buffered,
            std::span<const controller_middleware_descriptor>{}, std::span(&limit, std::size_t{1}));
        impl.finalize();
        const auto resolution = impl.route_table().resolve(http_known_method::get, "/limited");
        RUVIA_CHECK(resolution.resolved() != nullptr);
        RUVIA_CHECK_EQ(resolution.resolved()->route().max_request_body_bytes(), std::size_t{16});
    }

    {
        // A controller-wide and a route-specific declaration: the stricter wins,
        // regardless of which position it sits in.
        ruvia::detail::router router;
        auto& impl = ruvia::detail::router_impl::from(router);
        const auto controller_limit = ruvia::detail::make_middleware_descriptor<small_body_type>();
        const auto route_limit = ruvia::detail::make_middleware_descriptor<smaller_body_type>();
        impl.register_route(http_known_method::get, path("/both"),
            route_handler_type(nullptr, &dummy_handler), request_body_mode::buffered,
            std::span(&controller_limit, std::size_t{1}), std::span(&route_limit, std::size_t{1}));
        impl.finalize();
        const auto resolution = impl.route_table().resolve(http_known_method::get, "/both");
        RUVIA_CHECK(resolution.resolved() != nullptr);
        RUVIA_CHECK_EQ(resolution.resolved()->route().max_request_body_bytes(), std::size_t{8});
    }
}

RUVIA_TEST(finalized_route_table_carries_the_declared_deadline) {
    using slow_route_type = ruvia::deadline<30000>;
    using fast_route_type = ruvia::deadline<500>;

    {
        ruvia::detail::router router;
        auto& impl = ruvia::detail::router_impl::from(router);
        add_route(impl, "/plain");
        impl.finalize();
        const auto resolution = impl.route_table().resolve(http_known_method::get, "/plain");
        RUVIA_CHECK(resolution.resolved() != nullptr);
        RUVIA_CHECK_EQ(resolution.resolved()->route().deadline_ms(), std::int64_t{0});
    }

    {
        // controller-wide and route-specific: the stricter wins, not the nearer.
        // A route asking for MORE time than its controller declared does not get
        // it -- the one rule every app/route policy follows.
        ruvia::detail::router router;
        auto& impl = ruvia::detail::router_impl::from(router);
        const auto controller_deadline = ruvia::detail::make_middleware_descriptor<fast_route_type>();
        const auto route_deadline = ruvia::detail::make_middleware_descriptor<slow_route_type>();
        impl.register_route(http_known_method::get, path("/both"),
            route_handler_type(nullptr, &dummy_handler), request_body_mode::buffered,
            std::span(&controller_deadline, std::size_t{1}),
            std::span(&route_deadline, std::size_t{1}));
        impl.finalize();
        const auto resolution = impl.route_table().resolve(http_known_method::get, "/both");
        RUVIA_CHECK(resolution.resolved() != nullptr);
        RUVIA_CHECK_EQ(resolution.resolved()->route().deadline_ms(), std::int64_t{500});
    }
}

RUVIA_TEST(routing_dynamic_exact_match) {
    router r;
    add_route(r.impl_, "/users/:id");
    r.finalize();
    RUVIA_CHECK(r.matches("/users/42"));
    RUVIA_CHECK_EQ(r.param_of("/users/42"), std::string_view("42"));
}

RUVIA_TEST(routing_dynamic_trailing_slash_rejected) {
    router r;
    add_route(r.impl_, "/users/:id");
    r.finalize();
    // Strict matching: a trailing slash is a distinct (empty) segment, so this
    // must NOT collapse to /users/42.
    RUVIA_CHECK(!r.matches("/users/42/"));
}

RUVIA_TEST(routing_dynamic_empty_segment_rejected) {
    router r;
    add_route(r.impl_, "/a/:x");
    r.finalize();
    RUVIA_CHECK(r.matches("/a/b"));
    RUVIA_CHECK_EQ(r.param_of("/a/b"), std::string_view("b"));
    // A collapsed // must not silently match and drop the empty segment.
    RUVIA_CHECK(!r.matches("/a//b"));
    RUVIA_CHECK(!r.matches("/a/"));
}

RUVIA_TEST(routing_static_trailing_slash_rejected) {
    router r;
    add_route(r.impl_, "/users/list");
    add_route(r.impl_, "/users/:id");
    r.finalize();
    RUVIA_CHECK(r.matches("/users/list"));
    RUVIA_CHECK(!r.matches("/users/list/"));
    // /users/list still routes to the static entry (no param), not the dynamic one.
    RUVIA_CHECK_EQ(r.param_of("/users/list"), std::string_view("<none>"));
    // The dynamic sibling still works.
    RUVIA_CHECK_EQ(r.param_of("/users/99"), std::string_view("99"));
}

RUVIA_TEST(routing_wildcard_still_matches_tail) {
    router r;
    add_route(r.impl_, "/files/*");
    r.finalize();
    RUVIA_CHECK(r.matches("/files/a/b/c"));
    RUVIA_CHECK_EQ(r.param_of("/files/a/b/c"), std::string_view("a/b/c"));
}

RUVIA_TEST(routing_root_wildcard_shadows_param_conflict) {
    // A root wildcard shadows a root param sibling: must error at startup.
    RUVIA_CHECK(finalize_conflicts({"/*", "/:x"}));
    // Deeper-level shadowing already errored; guard it stays that way.
    RUVIA_CHECK(finalize_conflicts({"/a/*", "/a/:x"}));
    // Equivalent param shapes still conflict.
    RUVIA_CHECK(finalize_conflicts({"/users/:id", "/users/:name"}));
}

RUVIA_TEST(routing_root_wildcard_with_static_prefix_allowed) {
    // A root wildcard may coexist with routes distinguished by a static segment.
    RUVIA_CHECK(!finalize_conflicts({"/*", "/users/:id"}));
    RUVIA_CHECK(!finalize_conflicts({"/*", "/health"}));
}

RUVIA_TEST(routing_deep_wildcard_with_static_prefix_allowed) {
    // The static-sibling exemption holds at any depth, not just the root: "public" is a static
    // child tried before the wildcard, so "/files/public/5" -> :id route, "/files/x" -> wildcard,
    // deterministically. (Same situation as the allowed root case /* + /users/:id, one level down.)
    RUVIA_CHECK(!finalize_conflicts({"/files/*", "/files/public/:id"}));
    RUVIA_CHECK(!finalize_conflicts({"/files/*", "/files/list"}));
    RUVIA_CHECK(!finalize_conflicts({"/a/b/*", "/a/b/c/:id"}));
    RUVIA_CHECK(!finalize_conflicts({"/:section/*", "/health/live"}));
    RUVIA_CHECK(!finalize_conflicts({"/files/:bucket/*", "/files/public/:id"}));
    RUVIA_CHECK(!finalize_conflicts({"/:section/*", "/health/*"}));
    RUVIA_CHECK(!finalize_conflicts({"/:section/live", "/health/:probe"}));

    // Guards the fix must NOT regress (these genuinely shadow -> still conflicts):
    RUVIA_CHECK(
        finalize_conflicts({"/a/*", "/a/:x"}));          // wildcard vs param sibling at a shared node
    RUVIA_CHECK(finalize_conflicts({"/a/*", "/:x/b"}));  // after a static/param fork, the wildcard
    // steals the other route's direct-match path
    RUVIA_CHECK(finalize_conflicts({"/*", "/:x"}));                   // root wildcard vs param
    RUVIA_CHECK(finalize_conflicts({"/users/:id", "/users/:name"}));  // two params at one position
}

RUVIA_TEST(routing_allowed_wildcard_overlaps_resolve_to_static_priority_branch) {
    {
        router r;
        add_route(r.impl_, "/:section/*");
        add_route(r.impl_, "/health/*");
        r.finalize();
        RUVIA_CHECK_EQ(r.route_path_of("/health/live"), std::string_view("/health/*"));
        RUVIA_CHECK_EQ(r.route_path_of("/users/42"), std::string_view("/:section/*"));
    }

    {
        router r;
        add_route(r.impl_, "/files/:bucket/*");
        add_route(r.impl_, "/files/public/:id");
        r.finalize();
        RUVIA_CHECK_EQ(r.route_path_of("/files/public/5"), std::string_view("/files/public/:id"));
        RUVIA_CHECK_EQ(r.route_path_of("/files/private/a/b"), std::string_view("/files/:bucket/*"));
    }

    {
        router r;
        add_route(r.impl_, "/:section/live");
        add_route(r.impl_, "/health/:probe");
        r.finalize();
        RUVIA_CHECK_EQ(r.route_path_of("/health/live"), std::string_view("/health/:probe"));
        RUVIA_CHECK_EQ(r.route_path_of("/users/live"), std::string_view("/:section/live"));
    }
}

RUVIA_TEST(routing_head_fallback_respects_static_priority_overlap) {
    router r;
    add_route(r.impl_, "/:section/live");                           // implicit HEAD fallback
    add_route(r.impl_, http_known_method::head, "/health/:probe");  // explicit HEAD static branch
    r.finalize();

    RUVIA_CHECK_EQ(
        r.route_path_of(http_known_method::head, "/health/live"), std::string_view("/health/:probe"));
    RUVIA_CHECK_EQ(
        r.route_path_of(http_known_method::head, "/users/live"), std::string_view("/:section/live"));
}

RUVIA_TEST(routing_explicit_dynamic_head_overrides_exact_get_fallback) {
    router r;
    add_route(r.impl_, "/health/live");                               // implicit exact HEAD fallback
    add_route(r.impl_, http_known_method::head, "/:section/:probe");  // explicit HEAD route
    r.finalize();

    RUVIA_CHECK_EQ(r.route_path_of(http_known_method::head, "/health/live"),
        std::string_view("/:section/:probe"));
}

RUVIA_TEST(routing_explicit_head_precedes_more_specific_get_fallback) {
    router router;
    add_route(router.impl_, "/health/:probe");
    add_route(router.impl_, http_known_method::head, "/:section/live");
    router.finalize();
    RUVIA_CHECK_EQ(router.route_path_of(http_known_method::head, "/health/live"),
        std::string_view("/:section/live"));
    RUVIA_CHECK_EQ(router.route_path_of(http_known_method::head, "/health/ready"),
        std::string_view("/health/:probe"));
}

RUVIA_TEST(routing_405_allow_set_lists_the_other_registered_methods) {
    // A request whose method has no route for an existing path is a 405, and the
    // Allow set (RFC 7231 6.5.5) must list exactly the methods that DO have a route
    // for that path -- not methods belonging to other paths, and not the requested
    // method echoed back. This drives the Allow header and was only tested at the
    // route_resolution value level, never through the route-table computation.
    router r;
    add_route(r.impl_, http_known_method::get, "/a");
    add_route(r.impl_, http_known_method::post, "/a");
    add_route(r.impl_, http_known_method::put, "/b");
    r.finalize();

    const auto bit = [](http_known_method m) { return 1U << static_cast<unsigned>(m); };

    const auto res = r.impl_.route_table().resolve(http_known_method::delete_value, "/a");
    RUVIA_CHECK(res.resolved() == nullptr);
    const auto* method_not_allowed = res.method_not_allowed();
    RUVIA_CHECK(method_not_allowed != nullptr);  // /a exists for other methods -> 405
    const auto mask = method_not_allowed->allowed_methods();
    RUVIA_CHECK((mask & bit(http_known_method::get)) != 0);
    RUVIA_CHECK((mask & bit(http_known_method::post)) != 0);
    RUVIA_CHECK(
        (mask & bit(http_known_method::head)) != 0);         // implicit fallback to the GET route
    RUVIA_CHECK((mask & bit(http_known_method::put)) == 0);  // belongs to /b, not /a
    RUVIA_CHECK(
        (mask & bit(http_known_method::delete_value)) == 0);  // the requested method is not echoed back

    // A path with no route at all is a 404 (not found), never a 405.
    const auto missing = r.impl_.route_table().resolve(http_known_method::get, "/nope");
    RUVIA_CHECK(missing.resolved() == nullptr);
    RUVIA_CHECK(missing.method_not_allowed() == nullptr);
    RUVIA_CHECK(missing.not_found() != nullptr);
}

RUVIA_TEST(routing_options_only_resource_is_405_not_404) {
    // A path whose only registered method is OPTIONS must answer 405 (method known
    // but unsupported, RFC 9110 15.5.6), listing OPTIONS in Allow -- not 404, since
    // the resource exists. allowed_methods used to clear the OPTIONS bit and so
    // returned an empty set, making resolve() report not-found.
    router r;
    add_route(r.impl_, http_known_method::options, "/preflight");
    r.finalize();
    const auto bit = [](http_known_method m) { return 1U << static_cast<unsigned>(m); };

    const auto res = r.impl_.route_table().resolve(http_known_method::get, "/preflight");
    RUVIA_CHECK(res.resolved() == nullptr);
    RUVIA_CHECK(res.method_not_allowed() != nullptr);  // 405, not 404
    RUVIA_CHECK((res.method_not_allowed()->allowed_methods() & bit(http_known_method::options)) != 0);

    // The explicit OPTIONS route still handles an OPTIONS request to that path.
    const auto preflight = r.impl_.route_table().resolve(http_known_method::options, "/preflight");
    RUVIA_CHECK(preflight.resolved() != nullptr);
}

RUVIA_TEST(routing_options_asterisk_not_captured_by_wildcard_route) {
    // RFC 9110 7.1 / 9.3.7: "OPTIONS *" is a server-wide request, not a resource one.
    // A catch-all OPTIONS route must NOT capture it (the wildcard node otherwise
    // would), so it stays unresolved and dispatch emits the server-wide response.
    router r;
    add_route(r.impl_, http_known_method::options, "/*");
    add_route(r.impl_, http_known_method::get, "/*");
    r.finalize();

    const auto asterisk = r.impl_.route_table().resolve(http_known_method::options, "*");
    RUVIA_CHECK(asterisk.not_found() != nullptr);

    // A normal path still matches the catch-all: the short-circuit is only for "*".
    const auto wildcard = r.impl_.route_table().resolve(http_known_method::options, "/anything");
    RUVIA_CHECK(wildcard.resolved() != nullptr);
}

RUVIA_TEST(routing_rejects_duplicate_route_registration) {
    router r;
    add_route(r.impl_, http_known_method::get, "/x");
    std::array<std::string, 128> paths;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        paths[i] = (i % 2 == 0 ? "/" : "/long/registration/path/") + std::to_string(i);
        add_route(r.impl_, paths[i]);
    }
    // The same method+path registered twice is a duplicate: ambiguous routing is
    // rejected at registration rather than one route silently shadowing the other.
    bool threw = false;
    try {
        add_route(r.impl_, http_known_method::get, "/x");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    for (const auto& route_path : paths) {
        bool rejected = false;
        try {
            add_route(r.impl_, route_path);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
    // The SAME path under a DIFFERENT method is not a duplicate.
    add_route(r.impl_, http_known_method::post, "/x");
    r.finalize();
    RUVIA_CHECK(r.matches("/x"));
    for (const auto& route_path : paths) {
        RUVIA_CHECK(r.matches(route_path));
    }
}

RUVIA_TEST(routing_rejects_invalid_route_paths_at_registration) {
    const auto rejects = [](std::string_view route) {
        ruvia::detail::router router;
        auto& impl = ruvia::detail::router_impl::from(router);
        try {
            add_route(impl, http_known_method::get, route);
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
    router r;
    add_route(r.impl_, http_known_method::get, "/x");
    r.finalize();
    // The route table is immutable once finalized; a late registration must throw
    // rather than mutate the already-built table.
    bool threw = false;
    try {
        add_route(r.impl_, http_known_method::get, "/y");
    } catch (const std::logic_error&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(url_for_builds_paths_from_registered_patterns) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    for (const auto route : {"/users/:id", "/files/*", "/about", "/a/:x/b/:y"}) {
        impl.register_route(http_known_method::get, path(route), route_handler_type(nullptr, &ok_handler),
            request_body_mode::buffered, std::span<const controller_middleware_descriptor>{},
            std::span<const controller_middleware_descriptor>{});
    }
    impl.finalize();
    const auto& table_value = impl.route_table();
    auto* resource = std::pmr::get_default_resource();

    const auto url_for = [&](std::string_view pattern,
                             std::initializer_list<std::string_view> values) {
        return std::string(table_value.url_for(
            pattern, std::span<const std::string_view>(values.begin(), values.size()), resource));
    };

    RUVIA_CHECK_EQ(url_for("/users/:id", {"42"}), std::string("/users/42"));
    // Parameter values are percent-encoded as one path segment.
    RUVIA_CHECK_EQ(url_for("/users/:id", {"a b/c"}), std::string("/users/a%20b%2Fc"));
    // A wildcard value keeps its slashes; other bytes are still encoded.
    RUVIA_CHECK_EQ(url_for("/files/*", {"x/y z"}), std::string("/files/x/y%20z"));
    // An empty wildcard capture addresses the bare mount path.
    RUVIA_CHECK_EQ(url_for("/files/*", {""}), std::string("/files"));
    RUVIA_CHECK_EQ(url_for("/about", {}), std::string("/about"));
    RUVIA_CHECK_EQ(url_for("/a/:x/b/:y", {"1", "2"}), std::string("/a/1/b/2"));
    const std::string plain(4096, 'a');
    RUVIA_CHECK_EQ(url_for("/users/:id", {plain}), "/users/" + plain);
    const std::string mixed = "%" + plain + "/?#";
    RUVIA_CHECK_EQ(url_for("/users/:id", {mixed}), "/users/%25" + plain + "%2F%3F%23");
    RUVIA_CHECK_EQ(url_for("/files/*", {mixed}), "/files/%25" + plain + "/%3F%23");
    RUVIA_CHECK_EQ(url_for("/users/:id", {std::string_view("\0\xff", 2)}), std::string("/users/%00%FF"));
    RUVIA_CHECK_EQ(url_for("/users/:id", {"-._~!$&'()*+,;=:@"}), std::string("/users/-._~!$&'()*+,;=:@"));

    const auto throws = [&](std::string_view pattern,
                            std::initializer_list<std::string_view> values) {
        try {
            (void)url_for(pattern, values);
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
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_route(http_known_method::get, path("/echo"),
        route_handler_type(nullptr, &url_for_echo_handler), request_body_mode::buffered,
        std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
    impl.register_route(http_known_method::get, path("/users/:id"), route_handler_type(nullptr, &ok_handler),
        request_body_mode::buffered, std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
    impl.finalize();
    const auto& table_value = impl.route_table();

    RUVIA_CHECK_EQ(dispatch_on(table_value, "GET", "/echo").body_, std::string("/users/7"));
}
