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
    RUVIA_GET("/limited", limited, ruvia::RouteRateLimit<10, 1000>);
    RUVIA_POST("/small", small, ruvia::BodyLimit<16>);
    RUVIA_POST("/default", defaultBody);
    RUVIA_ROUTES_END
private:
    ruvia::Task<ruvia::HttpResponse> one(ruvia::Context& c) { co_return c.text("one"); }
    ruvia::Task<ruvia::HttpResponse> two(ruvia::Context& c) { co_return c.text("two"); }
    ruvia::Task<ruvia::HttpResponse> limited(ruvia::Context& c) { co_return c.text("limited"); }

    ruvia::Task<ruvia::HttpResponse> small(ruvia::Context& c) {
        const auto body = co_await c.req().text();
        co_return c.body(body);
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
    // RouteRateLimit<max, window> replaces the RUVIA_ROUTE_RATE_LIMIT macro,
    // whose only purpose was minting a named type to carry these two numbers.
    // TestApp has no rate limiter, so the middleware must pass the request
    // through rather than reject it.
    const auto limited = app.request(ruvia::TestRequest::get("/route-config/limited"));
    RUVIA_CHECK_EQ(limited.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(limited.body(), std::string_view("limited"));
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
}
