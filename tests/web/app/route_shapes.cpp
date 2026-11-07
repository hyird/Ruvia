#include <gtest/gtest.h>

// Overlapping dynamic route shapes register and dispatch by the matcher's
// precedence (static before parameter before wildcard, with backtracking), and
// path-scoped middleware follows the request path the router matched rather
// than the route pattern. Public API only.

#include <memory_resource>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/testing.h"

namespace {

// Stamps a header so a response shows whether this middleware ran at all.
class route_shapes_scope final {
public:
    explicit route_shapes_scope(std::string_view tag) noexcept
        : tag_(tag) {}

    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Route-Scope", tag_);
    }

private:
    std::string_view tag_;
};

class route_shapes_trace final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Route-Trace", "on");
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

// Refuses every request it guards, so a 403 shows the guarded route matched.
class route_shapes_deny final {
public:
    ruvia::task<void> handle(ruvia::context&, ruvia::next&) {
        throw ruvia::http_error({.status_ = ruvia::http_status::forbidden});
        co_return;
    }
};

// Literal routes guarded by route-level middleware beside parameter routes
// that would capture an encoded spelling of the same path, plus resources
// whose Allow sets the router must report completely.
class route_shapes_equivalence_controller final {
public:
    RUVIA_ROUTES_BEGIN(route_shapes_equivalence_controller)
    RUVIA_GET("/equiv/users/admin", guarded, route_shapes_deny);
    RUVIA_GET("/equiv/users/:id", user);
    RUVIA_GET("/equiv/users/admin/:tab", guarded, route_shapes_deny);
    RUVIA_GET("/equiv/users/:id/:tab", user);
    RUVIA_GET("/equiv/files/caf%C3%A9", guarded, route_shapes_deny);
    RUVIA_GET("/equiv/files/:name", file);
    RUVIA_GET("/equiv/stream", stream);
    RUVIA_GET("/equiv/colon/%3Aid", guarded, route_shapes_deny);
    RUVIA_GET("/equiv/colon/:id", user);
    RUVIA_GET("/equiv/star/%2A", guarded, route_shapes_deny);
    RUVIA_GET("/equiv/star/*", user);
    RUVIA_METHOD("EXT-A", "/equiv/many", user);
    RUVIA_METHOD("EXT-B", "/equiv/many", user);
    RUVIA_METHOD("EXT-C", "/equiv/many", user);
    RUVIA_METHOD("EXT-D", "/equiv/many", user);
    RUVIA_METHOD("EXT-E", "/equiv/many", user);
    RUVIA_METHOD("EXT-F", "/equiv/many", user);
    RUVIA_METHOD("EXT-G", "/equiv/many", user);
    RUVIA_METHOD("EXT-H", "/equiv/many", user);
    RUVIA_METHOD("EXT-I", "/equiv/many", user);
    RUVIA_METHOD("EXT-J", "/equiv/m%61ny", user);
    RUVIA_METHOD("Z-INDEX", "/indexed/z", guarded);
    RUVIA_METHOD("A-INDEX-TAIL", "/indexed/m", file);
    RUVIA_METHOD("A-INDEX", "/indexed/a%2Fb", user);
    RUVIA_METHOD("Z-INDEX", "/indexed/a/b", file);
    RUVIA_METHOD("A-INDEX", "/indexed/%61", guarded);
    RUVIA_METHOD("Z-INDEX", "/indexed/a%2fb", guarded);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> guarded(ruvia::context& c) {
        co_return c.body("guarded");
    }

    ruvia::task<ruvia::http_response> user(ruvia::context& c) {
        std::pmr::string body(c.arena());
        body.append("user:");
        body.append(c.req().param("id").value_or("-"));
        co_return c.body(std::move(body));
    }

    ruvia::task<ruvia::http_response> file(ruvia::context& c) {
        std::pmr::string body(c.arena());
        body.append("file:");
        body.append(c.req().param("name").value_or("-"));
        co_return c.body(std::move(body));
    }

    ruvia::task<void> stream(ruvia::context& c) {
        if (co_await c.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await c.stream().write("chunk");
    }
};

[[nodiscard]] std::multiset<std::string_view> allow_methods(std::string_view value) {
    std::multiset<std::string_view> methods;
    while (!value.empty()) {
        const auto comma = value.find(',');
        auto token = value.substr(0, comma);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
            token.remove_prefix(1);
        }
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
            token.remove_suffix(1);
        }
        methods.insert(token);
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return methods;
}

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
    c.draft().status(ruvia::http_status::not_found);
    co_return c.body("api-miss");
}

}  // namespace

TEST(Ruvia, route_shapes_static_side_wins_over_parameter_pair) {
    // "/shapes/a/*" and "/shapes/b/:z" both overlap "/shapes/:x/:y"; the literal
    // branch is tried first and the parameter pair serves everything else.
    ruvia::test_app app;
    EXPECT_EQ(body_of(app, "/shapes/a/b"), std::string("a-wild:b"));
    EXPECT_EQ(body_of(app, "/shapes/a/b/c"), std::string("a-wild:b/c"));
    EXPECT_EQ(body_of(app, "/shapes/a"), std::string("a-wild:"));
    EXPECT_EQ(body_of(app, "/shapes/b/q"), std::string("b-param:q"));
    EXPECT_EQ(body_of(app, "/shapes/c/d"), std::string("pair:c/d"));
}

TEST(Ruvia, route_shapes_parameter_wins_over_sibling_wildcard) {
    // A parameter is tried before the wildcard at the same node and the
    // wildcard takes over once the parameter branch fails.
    ruvia::test_app app;
    EXPECT_EQ(body_of(app, "/shapes/w/1"), std::string("w-param:1"));
    EXPECT_EQ(body_of(app, "/shapes/w/1/2"), std::string("w-wild:1/2"));
}

TEST(Ruvia, route_shapes_scoped_middleware_follows_the_request_path) {
    ruvia::test_app app;
    app.use_at<route_shapes_scope>({.prefix_ = "/admin"}, "admin");

    // One parameterized route serves paths on both sides of the scope.
    EXPECT_EQ(body_of(app, "/admin/panel"), std::string("panel:admin"));
    EXPECT_EQ(scope_of(app, "/admin/panel"), std::string("admin"));
    EXPECT_EQ(body_of(app, "/admin/panel/"), std::string("panel-slash:admin"));
    EXPECT_EQ(scope_of(app, "/admin/panel/"), std::string("admin"));
    EXPECT_EQ(body_of(app, "/other/panel"), std::string("panel:other"));
    EXPECT_EQ(scope_of(app, "/other/panel"), std::string("none"));
    // Whole segments only.
    EXPECT_EQ(scope_of(app, "/administrator/panel"), std::string("none"));
    // Scope segments compare percent-decoded, the form the handler reads its
    // parameter in, so an encoded spelling cannot step around the scope; an
    // encoded slash stays inside its segment.
    EXPECT_EQ(body_of(app, "/%61dmin/panel"), std::string("panel:admin"));
    EXPECT_EQ(scope_of(app, "/%61dmin/panel"), std::string("admin"));
    EXPECT_EQ(scope_of(app, "/admin%2Fx/panel"), std::string("none"));

    // A route entirely inside the scope and one entirely outside it.
    EXPECT_EQ(scope_of(app, "/admin/settings"), std::string("admin"));
    EXPECT_EQ(scope_of(app, "/shapes/c/d"), std::string("none"));
    // An encoded spelling of a static route is that route, under its scope.
    EXPECT_EQ(body_of(app, "/%61dmin/s%65ttings"), std::string("settings:admin"));
    EXPECT_EQ(scope_of(app, "/%61dmin/s%65ttings"), std::string("admin"));
}

TEST(Ruvia, route_shapes_scoped_middleware_checks_wildcard_captures) {
    ruvia::test_app app;
    app.use_at<route_shapes_scope>({.prefix_ = "/assets/public"}, "public");

    EXPECT_EQ(scope_of(app, "/assets/public"), std::string("public"));
    EXPECT_EQ(scope_of(app, "/assets/public/app.js"), std::string("public"));
    EXPECT_EQ(scope_of(app, "/assets/private/key"), std::string("none"));
    EXPECT_EQ(scope_of(app, "/assets/publicity"), std::string("none"));
    EXPECT_EQ(scope_of(app, "/assets"), std::string("none"));
}

TEST(Ruvia, route_shapes_trailing_slash_is_a_distinct_path_for_every_route_kind) {
    // RFC 3986 path identity: "/a" and "/a/" name different resources, so
    // static and dynamic routes alike neither alias nor shadow each other.
    ruvia::test_app app;
    EXPECT_EQ(body_of(app, "/slash/static"), std::string("static:bare"));
    EXPECT_EQ(body_of(app, "/slash/static/"), std::string("static:slash"));
    EXPECT_EQ(body_of(app, "/slash/42"), std::string("dyn:42"));
    EXPECT_EQ(body_of(app, "/slash/42/"), std::string("dyn-slash:42"));
    EXPECT_EQ(body_of(app, "/slash/42//"), std::string("unexpected status"));

    EXPECT_EQ(body_of(app, "/only/1"), std::string("only:1"));
    EXPECT_EQ(app.request(ruvia::test_request::get("/only/1/")).status(), ruvia::http_status::not_found);
    EXPECT_EQ(body_of(app, "/only-static"), std::string("only:static"));
    EXPECT_EQ(app.request(ruvia::test_request::get("/only-static/")).status(), ruvia::http_status::not_found);
}

TEST(Ruvia, route_shapes_url_for_keeps_the_registered_trailing_slash) {
    ruvia::test_app app;
    const auto links = app.request(ruvia::test_request::get("/slash-links"));
    EXPECT_EQ(links.status(), ruvia::http_status::ok);
    EXPECT_EQ(links.body(), "/slash/static/ /slash/7/ /slash/7");
    EXPECT_EQ(body_of(app, "/slash/7/"), std::string("dyn-slash:7"));
    EXPECT_EQ(body_of(app, "/slash/7"), std::string("dyn:7"));
}

TEST(Ruvia, route_shapes_non_http_absolute_target_is_rejected) {
    // A generic-scheme absolute-form target would carry a path no front proxy
    // routes like this server; it is a 400 before any route or scope runs.
    ruvia::test_app app;
    app.use_at<route_shapes_scope>({.prefix_ = "/admin"}, "admin");
    EXPECT_EQ(app.request(ruvia::test_request::get("x:admin/panel")).status(),
        ruvia::http_status::bad_request);
    EXPECT_EQ(app.request(ruvia::test_request::get("x:/admin/panel")).status(),
        ruvia::http_status::bad_request);
}

TEST(Ruvia, route_shapes_dot_segments_match_no_route) {
    // "/assets/public/../private/key" resolves to a resource under
    // "/assets/private" although its raw segments sit outside that scope.
    ruvia::test_app app;
    app.use_at<route_shapes_scope>({.prefix_ = "/assets/private"}, "private");
    for (const std::string_view path : {"/assets/public/../private/key", "/assets/public/%2E%2E/private/key",
             "/assets/./private/key", "/admin/panel/.", "/assets/%2e"}) {
        EXPECT_EQ(app.request(ruvia::test_request::get(path)).status(), ruvia::http_status::not_found);
    }
    // Dots inside a segment are ordinary names.
    EXPECT_EQ(body_of(app, "/assets/..key/.env"), std::string("asset:..key/.env"));
    EXPECT_EQ(scope_of(app, "/assets/private/key"), std::string("private"));
}

TEST(Ruvia, route_shapes_conditional_frames_keep_chain_order_and_neighbours) {
    ruvia::test_app app;
    app.use<route_shapes_trace>();
    app.use_at<route_shapes_scope>({.prefix_ = "/admin"}, "admin");

    // The app-wide frame after a skipped conditional frame still runs.
    const auto outside = app.request(ruvia::test_request::get("/other/panel"));
    EXPECT_EQ(outside.status(), ruvia::http_status::ok);
    EXPECT_TRUE(outside.header("X-Route-Trace").has_value());
    EXPECT_TRUE(!outside.header("X-Route-Scope").has_value());

    const auto inside = app.request(ruvia::test_request::get("/admin/panel"));
    EXPECT_EQ(inside.status(), ruvia::http_status::ok);
    EXPECT_TRUE(inside.header("X-Route-Trace").has_value());
    EXPECT_TRUE(inside.header("X-Route-Scope").has_value());
}

TEST(Ruvia, route_shapes_conditional_frames_keep_single_shot_next) {
    ruvia::test_app app;
    app.use_at<route_shapes_double_next>({.prefix_ = "/admin"});

    const auto inside = app.request(ruvia::test_request::get("/admin/panel"));
    EXPECT_EQ(inside.status(), ruvia::http_status::internal_server_error);
    const auto outside = app.request(ruvia::test_request::get("/other/panel"));
    EXPECT_EQ(outside.status(), ruvia::http_status::ok);
    EXPECT_EQ(outside.body(), std::string_view("panel:other"));
}

TEST(Ruvia, route_shapes_error_responses_survive_an_unfinished_unmatched_chain) {
    ruvia::test_app app;
    app.use<route_shapes_unfinished>();

    // The 404 runs the unmatched chain, which never finalizes; it must still
    // yield a response rather than an exception escaping the server, and that
    // error response is not renegotiated into a 406 for the refused coding.
    const auto response =
        app.request(ruvia::test_request::get("/missing").header("Accept-Encoding", "*;q=0"));
    EXPECT_EQ(response.status(), ruvia::http_status::internal_server_error);
}

TEST(Ruvia, route_shapes_fallback_prefixes_compare_percent_decoded) {
    ruvia::test_app app;
    app.on_not_found({.prefix_ = "/api", .handler_ = &route_shapes_api_miss});

    // An escaped spelling of a registered scope is the same scope.
    bool rejected = false;
    try {
        app.on_not_found({.prefix_ = "/%61pi/", .handler_ = &route_shapes_api_miss});
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) == "duplicate fallback prefix";
    }
    EXPECT_TRUE(rejected);

    const auto escaped = app.request(ruvia::test_request::get("/%61pi/missing"));
    EXPECT_EQ(escaped.status(), ruvia::http_status::not_found);
    EXPECT_EQ(escaped.body(), std::string_view("api-miss"));
}

TEST(Ruvia, route_shapes_encoded_spellings_select_the_literal_route_and_its_middleware) {
    // RFC 3986 6.2.2.2: "%61dmin" is "admin". A parameter route must not
    // capture an encoded spelling of a guarded literal route, neither a static
    // one nor a literal segment inside a dynamic one.
    ruvia::test_app app;
    for (const std::string_view path : {"/equiv/users/admin", "/equiv/users/%61dmin", "/equiv/users/adm%69n",
             "/equiv/users/%61%64%6D%69%6E", "/equiv/users/admin/x", "/equiv/users/%61dmin/x",
             "/equiv/users/adm%69n/x"}) {
        EXPECT_EQ(app.request(ruvia::test_request::get(path)).status(), ruvia::http_status::forbidden);
    }
    // Other names still reach the parameter route, decoded.
    EXPECT_EQ(body_of(app, "/equiv/users/%41dmin"), std::string("user:Admin"));
    EXPECT_EQ(body_of(app, "/equiv/users/bob/x"), std::string("user:bob"));
    // An encoded slash stays inside its segment: it names no literal route.
    EXPECT_EQ(body_of(app, "/equiv/users/admin%2Fx"), std::string("user:admin/x"));
}

TEST(Ruvia, route_shapes_hex_case_does_not_change_the_route) {
    // RFC 3986 6.2.2.1: hex digits in a percent-encoding are case-insensitive.
    ruvia::test_app app;
    for (const std::string_view path : {"/equiv/files/caf%C3%A9", "/equiv/files/caf%c3%a9", "/equiv/files/caf%C3%a9"}) {
        EXPECT_EQ(app.request(ruvia::test_request::get(path)).status(), ruvia::http_status::forbidden);
    }
    EXPECT_EQ(body_of(app, "/equiv/files/caf%C3%A8"), std::string("file:caf\xC3\xA8"));
}

TEST(Ruvia, route_shapes_encoded_colon_or_star_literal_is_not_a_parameter_or_wildcard) {
    // A literal segment starting with ':' or a literal '*' can only be spelled
    // encoded ("%3Aid", "%2A"); it is a route of its own beside the parameter
    // or wildcard sibling, not a duplicate registration of it.
    ruvia::test_app app;
    for (const std::string_view path : {"/equiv/colon/%3Aid", "/equiv/colon/%3aid", "/equiv/star/%2A"}) {
        EXPECT_EQ(app.request(ruvia::test_request::get(path)).status(), ruvia::http_status::forbidden);
    }
    EXPECT_EQ(body_of(app, "/equiv/colon/42"), std::string("user:42"));
    EXPECT_EQ(body_of(app, "/equiv/star/x"), std::string("user:-"));
}

TEST(Ruvia, route_shapes_allow_names_every_extension_method_of_a_resource) {
    // RFC 9110 15.5.6: Allow lists the resource's supported methods, however
    // many extension methods it has and however each route spelled the path.
    const std::multiset<std::string_view> expected{
        "OPTIONS", "EXT-A", "EXT-B", "EXT-C", "EXT-D", "EXT-E", "EXT-F", "EXT-G", "EXT-H", "EXT-I", "EXT-J"};
    ruvia::test_app app;
    const auto options = app.request(ruvia::test_request::options("/equiv/many"));
    EXPECT_EQ(options.status(), ruvia::http_status::no_content);
    EXPECT_EQ(allow_methods(options.header("Allow").value_or("")), expected);
    const auto get = app.request(ruvia::test_request::get("/equiv/m%61ny"));
    EXPECT_EQ(get.status(), ruvia::http_status::method_not_allowed);
    EXPECT_EQ(allow_methods(get.header("Allow").value_or("")), expected);
    const auto extension = app.request(ruvia::test_request::method("EXT-J", "/equiv/many"));
    EXPECT_EQ(extension.status(), ruvia::http_status::ok);
}

TEST(Ruvia, route_shapes_response_stream_get_route_advertises_head) {
    // RFC 9110 9.1/9.3.2: a GET resource supports HEAD, streaming or not.
    ruvia::test_app app;
    const auto options = app.request(ruvia::test_request::options("/equiv/stream"));
    EXPECT_EQ(options.status(), ruvia::http_status::no_content);
    EXPECT_EQ(options.header("Allow").value_or(""), std::string_view("GET, HEAD, OPTIONS"));
    const auto post = app.request(ruvia::test_request::post("/equiv/stream"));
    EXPECT_EQ(post.status(), ruvia::http_status::method_not_allowed);
    EXPECT_EQ(post.header("Allow").value_or(""), std::string_view("GET, HEAD, OPTIONS"));
}

TEST(Ruvia, extension_routes_keep_method_and_canonical_path_identity) {
    struct route_case final {
        std::string_view method_;
        std::string_view path_;
        std::string_view body_;
    };
    constexpr route_case cases[] = {
        {"A-INDEX", "/indexed/a%2fb", "user:-"},
        {"Z-INDEX", "/indexed/a%2Fb", "guarded"},
        {"Z-INDEX", "/indexed/a/b", "file:-"},
        {"A-INDEX", "/indexed/a", "guarded"},
        {"Z-INDEX", "/indexed/z", "guarded"},
        {"A-INDEX-TAIL", "/indexed/m", "file:-"}};
    ruvia::test_app app;
    for (const auto& item : cases) {
        SCOPED_TRACE(std::string(item.method_) + " " + std::string(item.path_));
        const auto response = app.request(ruvia::test_request::method(item.method_, item.path_));
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.body(), item.body_);
    }
}

TEST(Ruvia, extension_route_misses_distinguish_method_and_resource_identity) {
    struct miss_case final {
        std::string_view method_;
        std::string_view path_;
        ruvia::http_status_code status_;
    };
    const miss_case cases[] = {
        {"A-INDEX", "/0-indexed", ruvia::http_status::not_found},
        {"A-INDEX", "/zz-indexed", ruvia::http_status::not_found},
        {"A-INDEX", "/indexed/a%2fbb", ruvia::http_status::not_found},
        {"A-INDEX", "/indexed/m", ruvia::http_status::method_not_allowed},
        {"A-INDEX-", "/indexed/a", ruvia::http_status::not_implemented},
        {"a-index", "/indexed/a", ruvia::http_status::not_implemented},
        {"000-NOT-REGISTERED-INDEX", "/indexed/a", ruvia::http_status::not_implemented},
        {"ZZZ-NOT-REGISTERED-INDEX", "/indexed/a", ruvia::http_status::not_implemented}};
    ruvia::test_app app;
    for (const auto& item : cases) {
        SCOPED_TRACE(std::string(item.method_) + " " + std::string(item.path_));
        const auto response = app.request(ruvia::test_request::method(item.method_, item.path_));
        EXPECT_EQ(response.status(), item.status_);
        if (item.status_ != ruvia::http_status::method_not_allowed) {
            EXPECT_FALSE(response.header("Allow").has_value());
        }
    }
}

TEST(Ruvia, extension_route_allow_groups_only_canonical_equivalent_paths) {
    ruvia::test_app app;
    const auto encoded = app.request(ruvia::test_request::options("/indexed/a%2Fb"));
    EXPECT_EQ(encoded.status(), ruvia::http_status::no_content);
    EXPECT_EQ(allow_methods(encoded.header("Allow").value_or("")),
        (std::multiset<std::string_view>{"OPTIONS", "A-INDEX", "Z-INDEX"}));
    const auto split = app.request(ruvia::test_request::method("A-INDEX", "/indexed/a/b"));
    EXPECT_EQ(split.status(), ruvia::http_status::method_not_allowed);
    EXPECT_EQ(allow_methods(split.header("Allow").value_or("")),
        (std::multiset<std::string_view>{"OPTIONS", "Z-INDEX"}));
    const auto plain = app.request(ruvia::test_request::options("/indexed/%61"));
    EXPECT_EQ(plain.status(), ruvia::http_status::no_content);
    EXPECT_EQ(allow_methods(plain.header("Allow").value_or("")),
        (std::multiset<std::string_view>{"OPTIONS", "A-INDEX"}));
}
