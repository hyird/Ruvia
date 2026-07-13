#include "test_harness.h"

#include <concepts>
#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/HttpResponseHeadersAccess.h"
#include "ruvia/http/detail/server/HttpFinalResponseControlPlan.h"

namespace {

using ruvia::HttpProtocolVersion;
using ruvia::HttpResponse;
using ruvia::detail::Http1FinalResponseControl;
using ruvia::detail::Http2FinalResponseControl;
using ruvia::detail::HttpFinalResponseControlPlan;
using ruvia::detail::HttpFinalResponseControlPlanError;
using ruvia::detail::HttpFinalResponseControlPlanFailure;
using ruvia::detail::HttpFinalResponseControlPlanResult;
using ruvia::detail::httpFinalResponseControlPlan;

static_assert(!std::default_initializable<Http1FinalResponseControl>);
static_assert(!std::default_initializable<Http2FinalResponseControl>);
static_assert(!std::default_initializable<HttpFinalResponseControlPlan>);
static_assert(!std::default_initializable<HttpFinalResponseControlPlanFailure>);
static_assert(!std::default_initializable<HttpFinalResponseControlPlanResult>);

bool isFailure(
    const HttpResponse& response,
    HttpProtocolVersion version,
    HttpFinalResponseControlPlanError error) {
    const auto result = httpFinalResponseControlPlan(response, version);
    return result.plan() == nullptr &&
        result.failure() != nullptr &&
        result.failure()->error() == error;
}

void addUncheckedHeader(
    HttpResponse& response,
    std::string_view name,
    std::string_view value) {
    auto& headers = const_cast<ruvia::HttpResponseHeaders&>(
        response.headers());
    (void)ruvia::detail::HttpResponseHeadersAccess::add(
        headers,
        name,
        value,
        0);
}

}  // namespace

RUVIA_TEST(final_response_control_plan_owns_exact_protocol_alternative) {
    HttpResponse http1(std::pmr::get_default_resource());
    http1.header("Connection", "close");
    http1.header(
        "Connection",
        "Upgrade",
        HttpResponse::HeaderOptions{.append = true});
    http1.header("Upgrade", "websocket");

    const auto http1Result = httpFinalResponseControlPlan(
        http1,
        HttpProtocolVersion::kHttp11);
    RUVIA_CHECK(http1Result.failure() == nullptr);
    RUVIA_CHECK(http1Result.plan() != nullptr);
    if (http1Result.plan() != nullptr) {
        const auto* control = http1Result.plan()->http1();
        RUVIA_CHECK(control != nullptr);
        RUVIA_CHECK(http1Result.plan()->http2() == nullptr);
        if (control != nullptr) {
            RUVIA_CHECK(control->connectionOptions().close());
            RUVIA_CHECK(control->connectionOptions().upgrade());
            RUVIA_CHECK(control->upgradeProtocols().hasField());
            RUVIA_CHECK(control->upgradeProtocols().hasProtocol());
        }
    }

    HttpResponse http2(std::pmr::get_default_resource());
    http2.header("X-Trace", "ok");
    const auto http2Result = httpFinalResponseControlPlan(
        http2,
        HttpProtocolVersion::kHttp2);
    RUVIA_CHECK(http2Result.failure() == nullptr);
    RUVIA_CHECK(http2Result.plan() != nullptr);
    if (http2Result.plan() != nullptr) {
        RUVIA_CHECK(http2Result.plan()->http1() == nullptr);
        RUVIA_CHECK(http2Result.plan()->http2() != nullptr);
    }
}

RUVIA_TEST(final_response_control_failure_never_exposes_a_default_plan) {
    HttpResponse invalidConnection(std::pmr::get_default_resource());
    addUncheckedHeader(invalidConnection, "Connection", ", close");
    RUVIA_CHECK(isFailure(
        invalidConnection,
        HttpProtocolVersion::kHttp11,
        HttpFinalResponseControlPlanError::kInvalidConnectionField));

    HttpResponse invalidUpgrade(std::pmr::get_default_resource());
    addUncheckedHeader(invalidUpgrade, "Upgrade", "web socket");
    RUVIA_CHECK(isFailure(
        invalidUpgrade,
        HttpProtocolVersion::kHttp11,
        HttpFinalResponseControlPlanError::kInvalidUpgradeField));

    HttpResponse missingUpgrade(std::pmr::get_default_resource());
    missingUpgrade.status(426);
    RUVIA_CHECK(isFailure(
        missingUpgrade,
        HttpProtocolVersion::kHttp11,
        HttpFinalResponseControlPlanError::kUpgradeRequired));
    RUVIA_CHECK(isFailure(
        missingUpgrade,
        HttpProtocolVersion::kHttp2,
        HttpFinalResponseControlPlanError::kUpgradeUnavailable));
}

RUVIA_TEST(final_response_control_rejects_every_http2_connection_specific_field) {
    constexpr std::pair<std::string_view, std::string_view> fields[] = {
        {"Connection", "close"},
        {"Keep-Alive", "timeout=5"},
        {"Proxy-Connection", "keep-alive"},
        {"TE", "trailers"},
        {"Transfer-Encoding", "chunked"},
        {"Upgrade", "websocket"},
    };
    for (const auto& [name, value] : fields) {
        HttpResponse response(std::pmr::get_default_resource());
        response.header(name, value);
        RUVIA_CHECK(isFailure(
            response,
            HttpProtocolVersion::kHttp2,
            HttpFinalResponseControlPlanError::
                kConnectionSpecificFieldForbidden));
    }
}
