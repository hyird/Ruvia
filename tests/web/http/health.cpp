#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "ruvia/web/context.h"

#include "context_request_fixture.h"

TEST(Ruvia, health_responses_use_context_response_state) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        context_value.draft().header("X-Trace", "health");
        context_value.draft().set_cookie({.name_ = "probe", .value_ = "ok"});

        const auto response = ruvia::make_health_response(context_value);
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
        EXPECT_EQ(response.header("X-Trace"), std::string_view("health"));
        EXPECT_TRUE(
            response.header("Set-Cookie").value_or(std::string_view{}).starts_with("probe=ok;"));
        EXPECT_EQ(response.body_bytes(), std::string_view("{\"status\":\"ok\"}"));
        co_return;
    });
}

TEST(Ruvia, readiness_response_defaults_to_ready) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto response = ruvia::make_readiness_response(context_value);
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
        EXPECT_EQ(response.body_bytes(), std::string_view("{\"status\":\"ready\"}"));
        co_return;
    });
}

TEST(Ruvia, readiness_response_keeps_explicit_failure_status) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        context_value.draft().status(ruvia::http_status::created);

        const auto response =
            ruvia::make_readiness_response(context_value, {
                                                              .state_ = ruvia::readiness_state::unavailable,
                                                              .unavailable_reason_ = "database is unavailable",
                                                          });
        EXPECT_EQ(response.status(), ruvia::http_status::service_unavailable);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
        EXPECT_EQ(response.body_bytes(),
            std::string_view("{\"status\":\"not_ready\",\"reason\":\"database is unavailable\"}"));
        co_return;
    });
}

TEST(Ruvia, readiness_response_rejects_invalid_state) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        bool rejected = false;
        try {
            (void)ruvia::make_readiness_response(
                context_value, {.state_ = static_cast<ruvia::readiness_state>(std::uint8_t{0xFF})});
        } catch (const std::invalid_argument& exception) {
            rejected = std::string_view(exception.what()) == "readiness state is invalid";
        }
        EXPECT_TRUE(rejected);
        co_return;
    });
}
