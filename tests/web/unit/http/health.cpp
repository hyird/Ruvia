#include "ruvia/web/health.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_request.h"

#include "context/context_access.h"
#include "context_services_fixture.h"
#include "test_harness.h"

namespace {

[[nodiscard]] std::pair<ruvia::http_request, std::optional<ruvia::http_parse_error>> make_request(
    ruvia::request_memory& memory) {
    return ruvia::make_parsed_http_request("GET", "/", {}, {}, memory.resource());
}

[[nodiscard]] ruvia::context make_context(
    ruvia::request_memory& memory, ruvia::http_request& request) {
    return ruvia::detail::context_access::make(memory, request, ruvia::test::test_context_services());
}

}  // namespace

RUVIA_TEST(health_responses_use_context_response_state) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto [request, error] = make_request(memory);
    RUVIA_CHECK(!error.has_value());
    auto context_value = make_context(memory, request);
    context_value.header("X-Trace", "health");
    context_value.set_cookie({.name_ = "probe", .value_ = "ok"});

    const auto response = ruvia::make_health_response(context_value);
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(response.header("X-Trace"), std::string_view("health"));
    RUVIA_CHECK(
        response.header("Set-Cookie").value_or(std::string_view{}).starts_with("probe=ok;"));
    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("{\"status\":\"ok\"}"));
}

RUVIA_TEST(readiness_response_defaults_to_ready) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto [request, error] = make_request(memory);
    RUVIA_CHECK(!error.has_value());
    auto context_value = make_context(memory, request);

    const auto response = ruvia::make_readiness_response(context_value);
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("{\"status\":\"ready\"}"));
}

RUVIA_TEST(readiness_response_keeps_explicit_failure_status) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto [request, error] = make_request(memory);
    RUVIA_CHECK(!error.has_value());
    auto context_value = make_context(memory, request);
    context_value.status(ruvia::http_status::created);

    const auto response =
        ruvia::make_readiness_response(context_value, {
                                                          .state_ = ruvia::readiness_state::unavailable,
                                                          .unavailable_reason_ = "database is unavailable",
                                                      });
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::service_unavailable);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(response.body_bytes(),
        std::string_view("{\"status\":\"not_ready\",\"reason\":\"database is unavailable\"}"));
}

RUVIA_TEST(readiness_response_rejects_invalid_state) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto [request, error] = make_request(memory);
    RUVIA_CHECK(!error.has_value());
    auto context_value = make_context(memory, request);

    bool rejected = false;
    try {
        (void)ruvia::make_readiness_response(
            context_value, {.state_ = static_cast<ruvia::readiness_state>(std::uint8_t{0xFF})});
    } catch (const std::invalid_argument& exception) {
        rejected = std::string_view(exception.what()) == "readiness state is invalid";
    }
    RUVIA_CHECK(rejected);
}
