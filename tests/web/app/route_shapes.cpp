#include "test_harness.h"

// Overlapping dynamic route shapes register and dispatch by the matcher's
// precedence (static before parameter before wildcard, with backtracking), and
// path-scoped middleware follows the request path the router matched rather
// than the route pattern. Public API only.

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/testing.h"

namespace {

// Stamps a header so a response shows whether this middleware ran at all.
class route_shapes_scope final {
public:
    explicit route_shapes_scope(std::string_view tag) noexcept
        : tag_(tag) {}

    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Route-Scope", tag_);
    }

private:
    std::string_view tag_;
};

class route_shapes_trace final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Route-Trace", "on");
    }
};

// Violates the single-shot next() contract, so a response shows whether a
// conditionally scoped frame really ran under the same chain rules.
class route_shapes_double_next final {
public:
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
        co_await next_value();
    }
};

// Wraps unmatched requests but neither responds nor awaits next(): a chain
// that never finalizes the request.
class route_shapes_unfinished final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> handle(ruvia::context&, ruvia::next&) {
        co_return;
    }
};

class route_shapes_controller final {
public:
    RUVIA_ROUTES_BEGIN(route_shapes_controller)
    RUVIA_GET("/shapes/a/*", a_wildcard);
    RUVIA_GET("/shapes/:x/:y", pair);
    RUVIA_GET("/shapes/b/:z", b_param);
    RUVIA_GET("/shapes/w/:id", w_param);
    RUVIA_GET("/shapes/w/*", w_wildcard);
    RUVIA_GET("/:section/panel", panel);
    RUVIA_GET("/:section/panel/", panel_slash);
    RUVIA_GET("/admin/settings", settings);
    RUVIA_GET("/assets/*", asset);
    RUVIA_GET("/slash/static", static_bare);
    RUVIA_GET("/slash/static/", static_slash);
    RUVIA_GET("/slash/:id", dynamic_bare);
    RUVIA_GET("/slash/:id/", dynamic_slash);
    RUVIA_GET("/only/:id", only_dynamic);
    RUVIA_GET("/only-static", only_static);
    RUVIA_GET("/slash-links", slash_links);
    RUVIA_ROUTES_END

private:
    static ruvia::task<ruvia::http_response> labelled(
        ruvia::context& c, std::string_view label, std::string_view first, std::string_view second = {}) {
        std::pmr::string body(c.arena());
        body.append(label);
        body.push_back(':');
        body.append(first);
        if (!second.empty()) {
            body.push_back('/');
            body.append(second);
        }
        co_return c.body(std::move(body));
    }

    ruvia::task<ruvia::http_response> a_wildcard(ruvia::context& c) {
        return labelled(c, "a-wild", c.req().param("*").value_or("?"));
    }

    ruvia::task<ruvia::http_response> pair(ruvia::context& c) {
        return labelled(c, "pair", c.req().param("x").value_or("?"), c.req().param("y").value_or("?"));
    }

    ruvia::task<ruvia::http_response> b_param(ruvia::context& c) {
        return labelled(c, "b-param", c.req().param("z").value_or("?"));
    }

    ruvia::task<ruvia::http_response> w_param(ruvia::context& c) {
        return labelled(c, "w-param", c.req().param("id").value_or("?"));
    }

    ruvia::task<ruvia::http_response> w_wildcard(ruvia::context& c) {
        return labelled(c, "w-wild", c.req().param("*").value_or("?"));
    }

    ruvia::task<ruvia::http_response> panel(ruvia::context& c) {
        return labelled(c, "panel", c.req().param("section").value_or("?"));
    }

    ruvia::task<ruvia::http_response> panel_slash(ruvia::context& c) {
        return labelled(c, "panel-slash", c.req().param("section").value_or("?"));
    }

    ruvia::task<ruvia::http_response> settings(ruvia::context& c) {
        return labelled(c, "settings", "admin");
    }

    ruvia::task<ruvia::http_response> asset(ruvia::context& c) {
        return labelled(c, "asset", c.req().param("*").value_or("?"));
    }

    ruvia::task<ruvia::http_response> static_bare(ruvia::context& c) {
        return labelled(c, "static", "bare");
    }

    ruvia::task<ruvia::http_response> static_slash(ruvia::context& c) {
        return labelled(c, "static", "slash");
    }

    ruvia::task<ruvia::http_response> dynamic_bare(ruvia::context& c) {
        return labelled(c, "dyn", c.req().param("id").value_or("?"));
    }

    ruvia::task<ruvia::http_response> dynamic_slash(ruvia::context& c) {
        return labelled(c, "dyn-slash", c.req().param("id").value_or("?"));
    }

    ruvia::task<ruvia::http_response> only_dynamic(ruvia::context& c) {
        return labelled(c, "only", c.req().param("id").value_or("?"));
    }

    ruvia::task<ruvia::http_response> only_static(ruvia::context& c) {
        return labelled(c, "only", "static");
    }

    ruvia::task<ruvia::http_response> slash_links(ruvia::context& c) {
        std::pmr::string body(c.arena());
        body.append(c.url_for("/slash/static/"));
        body.push_back(' ');
        body.append(c.url_for("/slash/:id/", {"7"}));
        body.push_back(' ');
        body.append(c.url_for("/slash/:id", {"7"}));
        co_return c.body(std::move(body));
    }
};

// A non-200 answer reads as its own value so a mismatch is visible in the check.
[[nodiscard]] std::string body_of(ruvia::test_app& app, std::string_view path) {
    const auto response = app.request(ruvia::test_request::get(path));
    if (response.status() != ruvia::http_status::ok) {
        return "unexpected status";
    }
    return std::string(response.body());
}

[[nodiscard]] std::string scope_of(ruvia::test_app& app, std::string_view path) {
    const auto response = app.request(ruvia::test_request::get(path));
    if (response.status() != ruvia::http_status::ok) {
        return "unexpected status";
    }
    const auto scope = response.header("X-Route-Scope");
    return scope.has_value() ? std::string(*scope) : std::string("none");
}

ruvia::task<ruvia::http_response> route_shapes_api_miss(ruvia::context& c) {
    c.status(ruvia::http_status::not_found);
    co_return c.body("api-miss");
}

}  // namespace

RUVIA_TEST(route_shapes_static_side_wins_over_parameter_pair) {
    // "/shapes/a/*" and "/shapes/b/:z" both overlap "/shapes/:x/:y"; the literal
    // branch is tried first and the parameter pair serves everything else.
    ruvia::test_app app;
    RUVIA_CHECK_EQ(body_of(app, "/shapes/a/b"), std::string("a-wild:b"));
    RUVIA_CHECK_EQ(body_of(app, "/shapes/a/b/c"), std::string("a-wild:b/c"));
    RUVIA_CHECK_EQ(body_of(app, "/shapes/a"), std::string("a-wild:"));
    RUVIA_CHECK_EQ(body_of(app, "/shapes/b/q"), std::string("b-param:q"));
    RUVIA_CHECK_EQ(body_of(app, "/shapes/c/d"), std::string("pair:c/d"));
}

RUVIA_TEST(route_shapes_parameter_wins_over_sibling_wildcard) {
    // A parameter is tried before the wildcard at the same node and the
    // wildcard takes over once the parameter branch fails.
    ruvia::test_app app;
    RUVIA_CHECK_EQ(body_of(app, "/shapes/w/1"), std::string("w-param:1"));
    RUVIA_CHECK_EQ(body_of(app, "/shapes/w/1/2"), std::string("w-wild:1/2"));
}

RUVIA_TEST(route_shapes_scoped_middleware_follows_the_request_path) {
    ruvia::test_app app;
    app.use_at<route_shapes_scope>({.prefix_ = "/admin"}, "admin");

    // One parameterized route serves paths on both sides of the scope.
    RUVIA_CHECK_EQ(body_of(app, "/admin/panel"), std::string("panel:admin"));
    RUVIA_CHECK_EQ(scope_of(app, "/admin/panel"), std::string("admin"));
    RUVIA_CHECK_EQ(body_of(app, "/admin/panel/"), std::string("panel-slash:admin"));
    RUVIA_CHECK_EQ(scope_of(app, "/admin/panel/"), std::string("admin"));
    RUVIA_CHECK_EQ(body_of(app, "/other/panel"), std::string("panel:other"));
    RUVIA_CHECK_EQ(scope_of(app, "/other/panel"), std::string("none"));
    // Whole segments only.
    RUVIA_CHECK_EQ(scope_of(app, "/administrator/panel"), std::string("none"));
    // Scope segments compare percent-decoded, the form the handler reads its
    // parameter in, so an encoded spelling cannot step around the scope; an
    // encoded slash stays inside its segment.
    RUVIA_CHECK_EQ(body_of(app, "/%61dmin/panel"), std::string("panel:admin"));
    RUVIA_CHECK_EQ(scope_of(app, "/%61dmin/panel"), std::string("admin"));
    RUVIA_CHECK_EQ(scope_of(app, "/admin%2Fx/panel"), std::string("none"));

    // A route entirely inside the scope and one entirely outside it.
    RUVIA_CHECK_EQ(scope_of(app, "/admin/settings"), std::string("admin"));
    RUVIA_CHECK_EQ(scope_of(app, "/shapes/c/d"), std::string("none"));
}

RUVIA_TEST(route_shapes_scoped_middleware_checks_wildcard_captures) {
    ruvia::test_app app;
    app.use_at<route_shapes_scope>({.prefix_ = "/assets/public"}, "public");

    RUVIA_CHECK_EQ(scope_of(app, "/assets/public"), std::string("public"));
    RUVIA_CHECK_EQ(scope_of(app, "/assets/public/app.js"), std::string("public"));
    RUVIA_CHECK_EQ(scope_of(app, "/assets/private/key"), std::string("none"));
    RUVIA_CHECK_EQ(scope_of(app, "/assets/publicity"), std::string("none"));
    RUVIA_CHECK_EQ(scope_of(app, "/assets"), std::string("none"));
}

RUVIA_TEST(route_shapes_trailing_slash_is_a_distinct_path_for_every_route_kind) {
    // RFC 3986 path identity: "/a" and "/a/" name different resources, so
    // static and dynamic routes alike neither alias nor shadow each other.
    ruvia::test_app app;
    RUVIA_CHECK_EQ(body_of(app, "/slash/static"), std::string("static:bare"));
    RUVIA_CHECK_EQ(body_of(app, "/slash/static/"), std::string("static:slash"));
    RUVIA_CHECK_EQ(body_of(app, "/slash/42"), std::string("dyn:42"));
    RUVIA_CHECK_EQ(body_of(app, "/slash/42/"), std::string("dyn-slash:42"));
    RUVIA_CHECK_EQ(body_of(app, "/slash/42//"), std::string("unexpected status"));

    RUVIA_CHECK_EQ(body_of(app, "/only/1"), std::string("only:1"));
    RUVIA_CHECK_EQ(app.request(ruvia::test_request::get("/only/1/")).status(), ruvia::http_status::not_found);
    RUVIA_CHECK_EQ(body_of(app, "/only-static"), std::string("only:static"));
    RUVIA_CHECK_EQ(app.request(ruvia::test_request::get("/only-static/")).status(), ruvia::http_status::not_found);
}

RUVIA_TEST(route_shapes_url_for_keeps_the_registered_trailing_slash) {
    ruvia::test_app app;
    const auto links = app.request(ruvia::test_request::get("/slash-links"));
    RUVIA_CHECK_EQ(links.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(links.body(), "/slash/static/ /slash/7/ /slash/7");
    RUVIA_CHECK_EQ(body_of(app, "/slash/7/"), std::string("dyn-slash:7"));
    RUVIA_CHECK_EQ(body_of(app, "/slash/7"), std::string("dyn:7"));
}

RUVIA_TEST(route_shapes_non_http_absolute_target_is_rejected) {
    // A generic-scheme absolute-form target would carry a path no front proxy
    // routes like this server; it is a 400 before any route or scope runs.
    ruvia::test_app app;
    app.use_at<route_shapes_scope>({.prefix_ = "/admin"}, "admin");
    RUVIA_CHECK_EQ(app.request(ruvia::test_request::get("x:admin/panel")).status(),
        ruvia::http_status::bad_request);
    RUVIA_CHECK_EQ(app.request(ruvia::test_request::get("x:/admin/panel")).status(),
        ruvia::http_status::bad_request);
}

RUVIA_TEST(route_shapes_dot_segments_match_no_route) {
    // "/assets/public/../private/key" resolves to a resource under
    // "/assets/private" although its raw segments sit outside that scope.
    ruvia::test_app app;
    app.use_at<route_shapes_scope>({.prefix_ = "/assets/private"}, "private");
    for (const std::string_view path : {"/assets/public/../private/key", "/assets/public/%2E%2E/private/key",
             "/assets/./private/key", "/admin/panel/.", "/assets/%2e"}) {
        RUVIA_CHECK_EQ(app.request(ruvia::test_request::get(path)).status(), ruvia::http_status::not_found);
    }
    // Dots inside a segment are ordinary names.
    RUVIA_CHECK_EQ(body_of(app, "/assets/..key/.env"), std::string("asset:..key/.env"));
    RUVIA_CHECK_EQ(scope_of(app, "/assets/private/key"), std::string("private"));
}

RUVIA_TEST(route_shapes_conditional_frames_keep_chain_order_and_neighbours) {
    ruvia::test_app app;
    app.use<route_shapes_trace>();
    app.use_at<route_shapes_scope>({.prefix_ = "/admin"}, "admin");

    // The app-wide frame after a skipped conditional frame still runs.
    const auto outside = app.request(ruvia::test_request::get("/other/panel"));
    RUVIA_CHECK_EQ(outside.status(), ruvia::http_status::ok);
    RUVIA_CHECK(outside.header("X-Route-Trace").has_value());
    RUVIA_CHECK(!outside.header("X-Route-Scope").has_value());

    const auto inside = app.request(ruvia::test_request::get("/admin/panel"));
    RUVIA_CHECK_EQ(inside.status(), ruvia::http_status::ok);
    RUVIA_CHECK(inside.header("X-Route-Trace").has_value());
    RUVIA_CHECK(inside.header("X-Route-Scope").has_value());
}

RUVIA_TEST(route_shapes_conditional_frames_keep_single_shot_next) {
    ruvia::test_app app;
    app.use_at<route_shapes_double_next>({.prefix_ = "/admin"});

    const auto inside = app.request(ruvia::test_request::get("/admin/panel"));
    RUVIA_CHECK_EQ(inside.status(), ruvia::http_status::internal_server_error);
    const auto outside = app.request(ruvia::test_request::get("/other/panel"));
    RUVIA_CHECK_EQ(outside.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(outside.body(), std::string_view("panel:other"));
}

RUVIA_TEST(route_shapes_error_responses_survive_an_unfinished_unmatched_chain) {
    ruvia::test_app app;
    app.use<route_shapes_unfinished>();

    // The 404 runs the unmatched chain, which never finalizes; it must still
    // yield a response rather than an exception escaping the server, and that
    // error response is not renegotiated into a 406 for the refused coding.
    const auto response =
        app.request(ruvia::test_request::get("/missing").header("Accept-Encoding", "*;q=0"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::internal_server_error);
}

RUVIA_TEST(route_shapes_fallback_prefixes_compare_percent_decoded) {
    ruvia::test_app app;
    app.on_not_found({.prefix_ = "/api", .handler_ = &route_shapes_api_miss});

    // An escaped spelling of a registered scope is the same scope.
    bool rejected = false;
    try {
        app.on_not_found({.prefix_ = "/%61pi/", .handler_ = &route_shapes_api_miss});
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) == "duplicate fallback prefix";
    }
    RUVIA_CHECK(rejected);

    const auto escaped = app.request(ruvia::test_request::get("/%61pi/missing"));
    RUVIA_CHECK_EQ(escaped.status(), ruvia::http_status::not_found);
    RUVIA_CHECK_EQ(escaped.body(), std::string_view("api-miss"));
}
