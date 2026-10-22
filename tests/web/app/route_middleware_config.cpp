#include "test_harness.h"

// Route and controller middleware lists name types, so an entry there is
// default constructed. That does NOT mean such a middleware cannot be
// configured: the configuration travels as template parameters, which keeps it
// constexpr, keeps the type default constructible, and keeps the chain
// finalized at startup. These pin that, including the case a comma inside the
// template argument list makes look impossible through a variadic macro.
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/testing.h"

namespace route_middleware_config_test {
// Configuration carried in the type, so the middleware stays default
// constructible and the route macro needs no constructor arguments.
template <int level>
class configured_by_type final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Config", level == 2 ? "two" : "other");
    }
};

// Takes no configuration at all, so it is named bare in a route's list.
class plain_middleware final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Plain", "on");
    }
};

struct middleware_lifetime_observation final {
    unsigned live_{};
    unsigned constructed_{};
    unsigned destroyed_{};
};

class lifetime_middleware final {
public:
    lifetime_middleware(std::shared_ptr<middleware_lifetime_observation> observation, bool fail)
        : observation_(std::move(observation)) {
        if (fail) {
            throw std::runtime_error("middleware construction failed");
        }
        ++observation_->live_;
        ++observation_->constructed_;
    }

    ~lifetime_middleware() {
        --observation_->live_;
        ++observation_->destroyed_;
    }

    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
    }

private:
    std::shared_ptr<middleware_lifetime_observation> observation_;
};

// Two NTTPs: the comma lives inside the template argument list.
template <int a, int b>
class configured_by_two_values final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Config-Pair", a == 10 && b == 1000 ? "ok" : "bad");
    }
};

// A middleware may answer by itself: handle() returning task<http_response>
// produces the response without running the rest of the chain.
class response_middleware final {
public:
    ruvia::task<ruvia::http_response> handle(ruvia::context& c, ruvia::next&) {
        c.status(ruvia::http_status::accepted);
        co_return c.text("returned by middleware");
    }
};

class configured_by_type_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/route-config")
    RUVIA_ROUTES_BEGIN(configured_by_type_controller)
    RUVIA_GET("/one", one, configured_by_type<2>);
    RUVIA_GET("/two", two, configured_by_two_values<10, 1000>);
    RUVIA_GET("/limited", limited, ruvia::rate_limit<10, 60'000>);
    RUVIA_POST("/small", small, ruvia::body_limit<16>);
    RUVIA_POST("/small-decoded", small, ruvia::body_limit<64>);
    RUVIA_POST("/default", default_body);
    RUVIA_GET("/answered", one, response_middleware);
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

// A controller-group rule stacked over a route rule: each declaration keeps its
// own counter, whether or not the two carry the same numbers.
class stacked_rate_limit_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/stacked-limit", ruvia::rate_limit<4, 60'000>)
    RUVIA_ROUTES_BEGIN(stacked_rate_limit_controller)
    RUVIA_GET("/same", limited, ruvia::rate_limit<4, 60'000>);
    RUVIA_GET("/looser", limited, ruvia::rate_limit<6, 60'000>);
    RUVIA_ROUTES_END
private:
    ruvia::task<ruvia::http_response> limited(ruvia::context& c) {
        co_return c.text("limited");
    }
};
}  // namespace route_middleware_config_test

using namespace route_middleware_config_test;  // NOLINT(google-build-using-namespace)

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

RUVIA_TEST(stacked_route_rate_limits_count_each_declaration_once_per_request) {
    ruvia::test_app app;
    for (const std::string_view target : {"/stacked-limit/same", "/stacked-limit/looser"}) {
        // The group rule allows four requests; a shared counter would be
        // consumed twice per request and reject the third.
        for (int i = 0; i < 4; ++i) {
            const auto admitted = app.request(ruvia::test_request::get(target));
            RUVIA_CHECK_EQ(admitted.status(), ruvia::http_status::ok);
        }
        const auto rejected = app.request(ruvia::test_request::get(target));
        RUVIA_CHECK_EQ(rejected.status(), ruvia::http_status::too_many_requests);
        RUVIA_CHECK_EQ(
            rejected.header("X-RateLimit-Limit").value_or(std::string_view{}), std::string_view("4"));
    }
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

RUVIA_TEST(route_body_limit_bounds_the_decoded_representation) {
    ruvia::test_app app;
    const std::string plain(4096, 'x');
    auto encoded = ruvia::encode_http_content(
        ruvia::http_content_coding::gzip, plain, {.max_encoded_bytes_ = 64});
    const auto* encoded_bytes = std::get_if<std::pmr::string>(&encoded);
    RUVIA_CHECK(encoded_bytes != nullptr);
    if (encoded_bytes == nullptr) {
        return;
    }

    // The compressed body fits the route's ceiling on the wire, but the
    // route reads the decoded representation, which is far larger.
    const auto rejected = app.request(ruvia::test_request::post("/route-config/small-decoded")
            .header("Content-Encoding", "gzip")
            .body(*encoded_bytes));
    RUVIA_CHECK_EQ(rejected.status(), ruvia::http_status::content_too_large);

    // A sibling without the declaration decodes up to the app-wide ceiling.
    const auto accepted = app.request(ruvia::test_request::post("/route-config/default")
            .header("Content-Encoding", "gzip")
            .body(*encoded_bytes));
    RUVIA_CHECK_EQ(accepted.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(accepted.body(), std::string_view(plain));
}

RUVIA_TEST(app_rejects_route_only_body_limit_middleware) {
    ruvia::test_app app;
    bool body_limit_rejected = false;
    try {
        app.use<ruvia::body_limit<16>>();
    } catch (const std::invalid_argument&) {
        body_limit_rejected = true;
    }
    RUVIA_CHECK(body_limit_rejected);
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

RUVIA_TEST(route_middleware_returning_a_response_answers_without_the_handler) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::get("/route-config/answered"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::accepted);
    RUVIA_CHECK_EQ(response.body(), std::string_view("returned by middleware"));
}

RUVIA_TEST(test_app_releases_worker_middleware_after_startup_failure_and_normal_retirement) {
    const auto observation = std::make_shared<middleware_lifetime_observation>();
    {
        ruvia::test_app app;
        app.use<lifetime_middleware>(observation, false).use<lifetime_middleware>(observation, true);
        bool failed = false;
        try {
            (void)app.request(ruvia::test_request::get("/route-config/one"));
        } catch (const std::runtime_error&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(observation->constructed_, 1U);
        RUVIA_CHECK_EQ(observation->destroyed_, 1U);
        RUVIA_CHECK_EQ(observation->live_, 0U);
    }
    {
        ruvia::test_app app;
        app.use<lifetime_middleware>(observation, false);
        const auto response = app.request(ruvia::test_request::get("/route-config/one"));
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
        RUVIA_CHECK_EQ(observation->live_, 1U);
    }
    RUVIA_CHECK_EQ(observation->constructed_, 2U);
    RUVIA_CHECK_EQ(observation->destroyed_, 2U);
    RUVIA_CHECK_EQ(observation->live_, 0U);
}

RUVIA_TEST(test_app_releases_registered_middleware_arguments_with_the_app) {
    const auto observation = std::make_shared<middleware_lifetime_observation>();
    {
        ruvia::test_app app;
        app.use<lifetime_middleware>(observation, false);
        app.use_at<lifetime_middleware>({.prefix_ = "/route-config"}, observation, false);
        // Each registration retains its own copy of the arguments.
        RUVIA_CHECK_EQ(observation.use_count(), 3L);

        const auto response = app.request(ruvia::test_request::get("/route-config/one"));
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
        RUVIA_CHECK_EQ(observation->live_, 2U);

        // A registration the sealed app rejects does not keep its arguments.
        const auto running_count = observation.use_count();
        bool sealed = false;
        try {
            app.use<lifetime_middleware>(observation, false);
        } catch (const std::logic_error&) {
            sealed = true;
        }
        RUVIA_CHECK(sealed);
        RUVIA_CHECK_EQ(observation.use_count(), running_count);
    }
    RUVIA_CHECK_EQ(observation->live_, 0U);
    RUVIA_CHECK_EQ(observation.use_count(), 1L);
}

RUVIA_TEST(app_wide_route_declarations_are_rejected_instead_of_ignored) {
    // Admission reads body_limit from route and controller lists only,
    // so an app-wide registration would silently bound nothing.
    ruvia::test_app app;
    const auto rejects = [](auto&& register_value) {
        try {
            register_value();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    RUVIA_CHECK(rejects([&app] { app.use<ruvia::body_limit<16>>(); }));
    RUVIA_CHECK(rejects([&app] { app.use_at<ruvia::body_limit<16>>({.prefix_ = "/route-config"}); }));

    const auto response = app.request(ruvia::test_request::post("/route-config/default").body(std::string(64, 'x')));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
}
