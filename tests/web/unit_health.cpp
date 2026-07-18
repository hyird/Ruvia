#include "test_harness.h"

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/web/Health.h"
#include "ruvia/web/detail/http/ContextInternal.h"

#include <string_view>

namespace {

[[nodiscard]] ruvia::Context makeContext(
    ruvia::RequestMemory& memory,
    ruvia::HttpRequest& request) {
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    return ruvia::detail::ContextAccess::make(memory, request);
}

}  // namespace

RUVIA_TEST(health_responses_use_context_response_state) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = ruvia::detail::HttpRequestAccess::make();
    auto context = makeContext(memory, request);
    context.header("X-Trace", "health");
    context.setCookie("probe", "ok");

    const auto response = ruvia::makeHealthResponse(context);
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{200});
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(response.header("X-Trace"), std::string_view("health"));
    RUVIA_CHECK(response.header("Set-Cookie").value_or(std::string_view{}).starts_with("probe=ok;"));
    RUVIA_CHECK_EQ(
        ruvia::detail::responseBody(response).bytes(),
        std::string_view("{\"status\":\"ok\"}"));
}

RUVIA_TEST(ready_response_keeps_explicit_failure_status) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = ruvia::detail::HttpRequestAccess::make();
    auto context = makeContext(memory, request);
    context.status(201);

    const auto response = ruvia::makeReadyResponse(context, false, "database is unavailable");
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{503});
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(
        ruvia::detail::responseBody(response).bytes(),
        std::string_view("{\"status\":\"not_ready\",\"reason\":\"database is unavailable\"}"));
}
