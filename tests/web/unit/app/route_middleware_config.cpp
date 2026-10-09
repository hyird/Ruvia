#include "test_harness.h"

// Route and controller middleware lists name types, so an entry there is
// default constructed. That does NOT mean such a middleware cannot be
// configured: the configuration travels as template parameters, which keeps it
// constexpr, keeps the type default constructible, and keeps the chain
// finalized at startup. These pin that, including the case a comma inside the
// template argument list makes look impossible through a variadic macro.
#include <string>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/body_limit.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/deadline.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/testing.h"

namespace {
// Configuration carried in the type, so the middleware stays default
// constructible and the route macro needs no constructor arguments.
template <int level>
class configured_by_type final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Config", level == 2 ? "two" : "other");
    }
};

// Takes no configuration at all, so it is named bare in a route's list.
class plain_middleware final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Plain", "on");
    }
};

// Two NTTPs: the comma lives inside the template argument list.
template <int a, int b>
class configured_by_two_values final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Config-Pair", a == 10 && b == 1000 ? "ok" : "bad");
    }
};

class configured_by_type_controller final : public ruvia::controller<configured_by_type_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/route-config")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/one", one, configured_by_type<2>);
    RUVIA_GET("/two", two, configured_by_two_values<10, 1000>);
    RUVIA_GET("/limited", limited, ruvia::rate_limit<10, 60'000>);
    RUVIA_POST("/small", small, ruvia::body_limit<16>);
    RUVIA_POST("/default", default_body);
    RUVIA_GET("/deadline", deadline, ruvia::deadline<100>);
    // Unparameterized and parameterized entries in ONE list: both are types, so
    // the typename pack takes them together and a bare name needs no braces.
    RUVIA_POST("/mixed", mixed, configured_by_type<2>, plain_middleware, ruvia::body_limit<32>,
        ruvia::rate_limit<10, 1000>);
    RUVIA_ROUTES_END
private:
    ruvia::task<ruvia::http_response> one(ruvia::context& c) {
        co_return c.text("one");
    }
    ruvia::task<ruvia::http_response> two(ruvia::context& c) {
        co_return c.text("two");
    }
    ruvia::task<ruvia::http_response> limited(ruvia::context& c) {
        co_return c.text("limited");
    }
    ruvia::task<ruvia::http_response> deadline(ruvia::context& c) {
        co_return c.text("deadline");
    }

    ruvia::task<ruvia::http_response> small(ruvia::context& c) {
        const auto body = co_await c.req().text();
        co_return c.body(body);
    }

    ruvia::task<ruvia::http_response> mixed(ruvia::context& c) {
        co_return c.text("mixed");
    }

    ruvia::task<ruvia::http_response> default_body(ruvia::context& c) {
        const auto body = co_await c.req().text();
        co_return c.body(body);
    }
};
}  // namespace

RUVIA_TEST(route_middleware_carries_its_configuration_in_the_type) {
    ruvia::test_app app;
    const auto one = app.request(ruvia::test_request::get("/route-config/one"));
    RUVIA_CHECK_EQ(one.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(one.header("X-Config").value_or(std::string_view{}), std::string_view("two"));

    // Two template arguments: the route macro is variadic, so the comma splits
    // its argument list -- but pasting __VA_ARGS__ back into the template
    // argument list rejoins them, which is why no wrapper type is needed.
    const auto two = app.request(ruvia::test_request::get("/route-config/two"));
    RUVIA_CHECK_EQ(
        two.header("X-Config-Pair").value_or(std::string_view{}), std::string_view("ok"));
}

RUVIA_TEST(route_rate_limit_is_configured_without_a_generated_type) {
    ruvia::test_app app;
    // rate_limit<max, window> replaces the RUVIA_ROUTE_RATE_LIMIT macro,
    // whose only purpose was minting a named type to carry these two numbers.
    for (int i = 0; i < 10; ++i) {
        const auto limited = app.request(ruvia::test_request::get("/route-config/limited"));
        RUVIA_CHECK_EQ(limited.status(), ruvia::http_status::ok);
        RUVIA_CHECK_EQ(limited.body(), std::string_view("limited"));
    }
    const auto rejected = app.request(ruvia::test_request::get("/route-config/limited"));
    RUVIA_CHECK_EQ(rejected.status(), ruvia::http_status::too_many_requests);
    RUVIA_CHECK_EQ(
        rejected.header("X-RateLimit-Limit").value_or(std::string_view{}), std::string_view("10"));
}

RUVIA_TEST(route_body_limit_is_declared_through_the_type) {
    ruvia::test_app app;

    // Within the route's ceiling.
    const auto small =
        app.request(ruvia::test_request::post("/route-config/small").body("0123456789"));
    RUVIA_CHECK_EQ(small.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(small.body(), std::string_view("0123456789"));

    // A sibling route without the declaration keeps the app-wide ceiling, so
    // the same body that the limited route would reject is fine here.
    const auto oversize_for_route = std::string(64, 'x');
    const auto unlimited =
        app.request(ruvia::test_request::post("/route-config/default").body(oversize_for_route));
    RUVIA_CHECK_EQ(unlimited.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(unlimited.body().size(), std::size_t{64});

    const auto rejected =
        app.request(ruvia::test_request::post("/route-config/small").body(oversize_for_route));
    RUVIA_CHECK_EQ(rejected.status(), ruvia::http_status::content_too_large);
}

RUVIA_TEST(test_app_runs_routes_with_deadlines) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::get("/route-config/deadline"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.body(), std::string_view("deadline"));
}

RUVIA_TEST(route_middleware_list_mixes_bare_and_parameterized_types) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::post("/route-config/mixed").body("x"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.body(), std::string_view("mixed"));
    // The bare entry ran alongside the parameterized ones.
    RUVIA_CHECK_EQ(response.header("X-Plain").value_or(std::string_view{}), std::string_view("on"));
    RUVIA_CHECK_EQ(
        response.header("X-Config").value_or(std::string_view{}), std::string_view("two"));
}
