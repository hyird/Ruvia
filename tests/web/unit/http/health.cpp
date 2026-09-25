#include "ruvia/web/Health.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"

#include "context_services_fixture.h"
#include "test_harness.h"

namespace {

[[nodiscard]] std::pair<ruvia::HttpRequest, std::optional<ruvia::HttpParseError>> makeRequest(
    ruvia::RequestMemory& memory) {
    return ruvia::makeParsedHttpRequest("GET", "/", {}, {}, memory.resource());
}

[[nodiscard]] ruvia::Context makeContext(
    ruvia::RequestMemory& memory, ruvia::HttpRequest& request) {
    return ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
}

}  // namespace

RUVIA_TEST(health_responses_use_context_response_state) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto [request, error] = makeRequest(memory);
    RUVIA_CHECK(!error.has_value());
    auto context = makeContext(memory, request);
    context.header("X-Trace", "health");
    context.setCookie({.name = "probe", .value = "ok"});

    const auto response = ruvia::makeHealthResponse(context);
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(response.header("X-Trace"), std::string_view("health"));
    RUVIA_CHECK(
        response.header("Set-Cookie").value_or(std::string_view{}).starts_with("probe=ok;"));
    RUVIA_CHECK_EQ(response.bodyBytes(), std::string_view("{\"status\":\"ok\"}"));
}

RUVIA_TEST(readiness_response_defaults_to_ready) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto [request, error] = makeRequest(memory);
    RUVIA_CHECK(!error.has_value());
    auto context = makeContext(memory, request);

    const auto response = ruvia::makeReadinessResponse(context);
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(response.bodyBytes(), std::string_view("{\"status\":\"ready\"}"));
}

RUVIA_TEST(readiness_response_keeps_explicit_failure_status) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto [request, error] = makeRequest(memory);
    RUVIA_CHECK(!error.has_value());
    auto context = makeContext(memory, request);
    context.status(ruvia::http_status::kCreated);

    const auto response =
        ruvia::makeReadinessResponse(context, {
                                                  .state = ruvia::ReadinessState::kUnavailable,
                                                  .unavailableReason = "database is unavailable",
                                              });
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kServiceUnavailable);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(response.bodyBytes(),
        std::string_view("{\"status\":\"not_ready\",\"reason\":\"database is unavailable\"}"));
}

RUVIA_TEST(readiness_response_rejects_invalid_state) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto [request, error] = makeRequest(memory);
    RUVIA_CHECK(!error.has_value());
    auto context = makeContext(memory, request);

    bool rejected = false;
    try {
        (void)ruvia::makeReadinessResponse(
            context, {.state = static_cast<ruvia::ReadinessState>(std::uint8_t{0xFF})});
    } catch (const std::invalid_argument& exception) {
        rejected = std::string_view(exception.what()) == "readiness state is invalid";
    }
    RUVIA_CHECK(rejected);
}
