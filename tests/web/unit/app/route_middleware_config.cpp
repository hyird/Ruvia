#include "test_harness.h"

// Route and controller middleware lists name types, so an entry there is
// default constructed. That does NOT mean such a middleware cannot be
// configured: the configuration travels as template parameters, which keeps it
// constexpr, keeps the type default constructible, and keeps the chain
// finalized at startup. These pin that, including the case a comma inside the
// template argument list makes look impossible through a variadic macro.
#include <string>
#include <string_view>
#include "ruvia/web/App.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/BodyLimit.h"
#include "ruvia/web/Deadline.h"
#include "ruvia/web/RateLimit.h"
#include "ruvia/web/Testing.h"

namespace {
// Configuration carried in the type, so the middleware stays default
// constructible and the route macro needs no constructor arguments.
template <int Level>
class ConfiguredByType final : public ruvia::Middleware<ConfiguredByType<Level>> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Config", Level == 2 ? "two" : "other");
    }
};

// Takes no configuration at all, so it is named bare in a route's list.
class PlainMiddleware final : public ruvia::Middleware<PlainMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Plain", "on");
    }
};

// Two NTTPs: the comma lives inside the template argument list.
template <int A, int B>
class ConfiguredByTwoValues final : public ruvia::Middleware<ConfiguredByTwoValues<A, B>> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Config-Pair", A == 10 && B == 1000 ? "ok" : "bad");
    }
};

class ConfiguredByTypeController final : public ruvia::Controller<ConfiguredByTypeController> {
public:
    RUVIA_CONTROLLER_GROUP("/route-config")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/one", one, ConfiguredByType<2>);
    RUVIA_GET("/two", two, ConfiguredByTwoValues<10, 1000>);
    RUVIA_GET("/limited", limited, ruvia::RateLimit<10, 60'000>);
    RUVIA_POST("/small", small, ruvia::BodyLimit<16>);
    RUVIA_POST("/default", defaultBody);
    RUVIA_GET("/deadline", deadline, ruvia::Deadline<100>);
    // Unparameterized and parameterized entries in ONE list: both are types, so
    // the typename pack takes them together and a bare name needs no braces.
    RUVIA_POST("/mixed", mixed, ConfiguredByType<2>, PlainMiddleware, ruvia::BodyLimit<32>, ruvia::RateLimit<10, 1000>);
    RUVIA_ROUTES_END
private:
    ruvia::Task<ruvia::HttpResponse> one(ruvia::Context& c) {
        co_return c.text("one");
    }
    ruvia::Task<ruvia::HttpResponse> two(ruvia::Context& c) {
        co_return c.text("two");
    }
    ruvia::Task<ruvia::HttpResponse> limited(ruvia::Context& c) {
        co_return c.text("limited");
    }
    ruvia::Task<ruvia::HttpResponse> deadline(ruvia::Context& c) {
        co_return c.text("deadline");
    }

    ruvia::Task<ruvia::HttpResponse> small(ruvia::Context& c) {
        const auto body = co_await c.req().text();
        co_return c.body(body);
    }

    ruvia::Task<ruvia::HttpResponse> mixed(ruvia::Context& c) {
        co_return c.text("mixed");
    }

    ruvia::Task<ruvia::HttpResponse> defaultBody(ruvia::Context& c) {
        const auto body = co_await c.req().text();
        co_return c.body(body);
    }
};
}  // namespace

RUVIA_TEST(route_middleware_carries_its_configuration_in_the_type) {
    ruvia::TestApp app;
    const auto one = app.request(ruvia::TestRequest::get("/route-config/one"));
    RUVIA_CHECK_EQ(one.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(one.header("X-Config").value_or(std::string_view{}), std::string_view("two"));

    // Two template arguments: the route macro is variadic, so the comma splits
    // its argument list -- but pasting __VA_ARGS__ back into the template
    // argument list rejoins them, which is why no wrapper type is needed.
    const auto two = app.request(ruvia::TestRequest::get("/route-config/two"));
    RUVIA_CHECK_EQ(two.header("X-Config-Pair").value_or(std::string_view{}), std::string_view("ok"));
}

RUVIA_TEST(route_rate_limit_is_configured_without_a_generated_type) {
    ruvia::TestApp app;
    // RateLimit<max, window> replaces the RUVIA_ROUTE_RATE_LIMIT macro,
    // whose only purpose was minting a named type to carry these two numbers.
    for (int i = 0; i < 10; ++i) {
        const auto limited = app.request(ruvia::TestRequest::get("/route-config/limited"));
        RUVIA_CHECK_EQ(limited.status(), ruvia::http_status::kOk);
        RUVIA_CHECK_EQ(limited.body(), std::string_view("limited"));
    }
    const auto rejected = app.request(ruvia::TestRequest::get("/route-config/limited"));
    RUVIA_CHECK_EQ(rejected.status(), ruvia::http_status::kTooManyRequests);
    RUVIA_CHECK_EQ(rejected.header("X-RateLimit-Limit").value_or(std::string_view{}), std::string_view("10"));
}

RUVIA_TEST(route_body_limit_is_declared_through_the_type) {
    ruvia::TestApp app;

    // Within the route's ceiling.
    const auto small = app.request(ruvia::TestRequest::post("/route-config/small").body("0123456789"));
    RUVIA_CHECK_EQ(small.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(small.body(), std::string_view("0123456789"));

    // A sibling route without the declaration keeps the app-wide ceiling, so
    // the same body that the limited route would reject is fine here.
    const auto oversizeForRoute = std::string(64, 'x');
    const auto unlimited = app.request(ruvia::TestRequest::post("/route-config/default").body(oversizeForRoute));
    RUVIA_CHECK_EQ(unlimited.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(unlimited.body().size(), std::size_t{64});

    const auto rejected = app.request(ruvia::TestRequest::post("/route-config/small").body(oversizeForRoute));
    RUVIA_CHECK_EQ(rejected.status(), ruvia::http_status::kContentTooLarge);
}

RUVIA_TEST(test_app_runs_routes_with_deadlines) {
    ruvia::TestApp app;
    const auto response = app.request(ruvia::TestRequest::get("/route-config/deadline"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(response.body(), std::string_view("deadline"));
}

RUVIA_TEST(route_middleware_list_mixes_bare_and_parameterized_types) {
    ruvia::TestApp app;
    const auto response = app.request(ruvia::TestRequest::post("/route-config/mixed").body("x"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(response.body(), std::string_view("mixed"));
    // The bare entry ran alongside the parameterized ones.
    RUVIA_CHECK_EQ(response.header("X-Plain").value_or(std::string_view{}), std::string_view("on"));
    RUVIA_CHECK_EQ(response.header("X-Config").value_or(std::string_view{}), std::string_view("two"));
}
