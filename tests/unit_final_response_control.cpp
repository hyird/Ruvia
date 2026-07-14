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
using ruvia::detail::HttpFinalResponseControlPlanError;
using ruvia::detail::HttpFinalResponseControlPlanFailure;
using ruvia::detail::HttpFinalResponseControlPlanResult;
using ruvia::detail::httpFinalResponseControlPlan;

static_assert(!std::default_initializable<Http1FinalResponseControl>);
static_assert(!std::default_initializable<Http2FinalResponseControl>);
static_assert(!std::default_initializable<HttpFinalResponseControlPlanFailure>);
static_assert(!std::default_initializable<HttpFinalResponseControlPlanResult>);
static_assert(std::same_as<
    decltype(std::declval<const Http1FinalResponseControl&>()
        .connectionOptions()),
    ruvia::detail::HttpConnectionOptions>);
static_assert(std::same_as<
    decltype(std::declval<const Http1FinalResponseControl&&>()
        .connectionOptions()),
    ruvia::detail::HttpConnectionOptions>);
static_assert(std::same_as<
    decltype(std::declval<const Http1FinalResponseControl&>()
        .upgradeProtocols()),
    ruvia::detail::HttpUpgradeProtocols>);
static_assert(std::same_as<
    decltype(std::declval<const Http1FinalResponseControl&&>()
        .upgradeProtocols()),
    ruvia::detail::HttpUpgradeProtocols>);

bool isFailure(
    const HttpResponse& response,
    HttpProtocolVersion version,
    HttpFinalResponseControlPlanError error) {
    const auto result = httpFinalResponseControlPlan(response, version);
    return result.http1() == nullptr &&
        result.http2() == nullptr &&
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

RUVIA_TEST(final_response_control_result_owns_exact_protocol_alternative) {
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
    const auto* http1Control = http1Result.http1();
    RUVIA_CHECK(http1Control != nullptr);
    RUVIA_CHECK(http1Result.http2() == nullptr);
    if (http1Control != nullptr) {
        RUVIA_CHECK(http1Control->connectionOptions().close());
        RUVIA_CHECK(http1Control->connectionOptions().upgrade());
        RUVIA_CHECK(http1Control->upgradeProtocols().hasField());
        RUVIA_CHECK(http1Control->upgradeProtocols().hasProtocol());
    }

    HttpResponse http2(std::pmr::get_default_resource());
    http2.header("X-Trace", "ok");
    const auto http2Result = httpFinalResponseControlPlan(
        http2,
        HttpProtocolVersion::kHttp2);
    RUVIA_CHECK(http2Result.failure() == nullptr);
    RUVIA_CHECK(http2Result.http1() == nullptr);
    RUVIA_CHECK(http2Result.http2() != nullptr);
}

RUVIA_TEST(final_response_control_failure_never_exposes_protocol_alternative) {
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
