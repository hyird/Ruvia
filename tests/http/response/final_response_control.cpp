#include "test_harness.h"

#include <concepts>
#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/response/HttpResponseHeadersAccess.h"
#include "ruvia/http/detail/server/HttpFinalResponseControlPlan.h"

namespace {

using ruvia::HttpResponse;
using ruvia::detail::Http1FinalResponseControl;
using ruvia::detail::Http1FinalResponseControlPlanError;
using ruvia::detail::Http1FinalResponseControlPlanFailure;
using ruvia::detail::Http1FinalResponseControlPlanResult;
using ruvia::detail::Http2FinalResponseControl;
using ruvia::detail::Http2FinalResponseControlPlanError;
using ruvia::detail::Http2FinalResponseControlPlanFailure;
using ruvia::detail::Http2FinalResponseControlPlanResult;
using ruvia::detail::http1FinalResponseControlPlan;
using ruvia::detail::http2FinalResponseControlPlan;

static_assert(!std::default_initializable<Http1FinalResponseControl>);
static_assert(!std::default_initializable<Http2FinalResponseControl>);
static_assert(!std::default_initializable<Http1FinalResponseControlPlanFailure>);
static_assert(!std::default_initializable<Http2FinalResponseControlPlanFailure>);
static_assert(!std::default_initializable<Http1FinalResponseControlPlanResult>);
static_assert(!std::default_initializable<Http2FinalResponseControlPlanResult>);
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

bool isHttp1Failure(
    const HttpResponse& response,
    Http1FinalResponseControlPlanError error) {
    const auto result = http1FinalResponseControlPlan(response);
    return result.control() == nullptr &&
        result.failure() != nullptr &&
        result.failure()->error() == error;
}

bool isHttp2Failure(
    const HttpResponse& response,
    Http2FinalResponseControlPlanError error) {
    const auto result = http2FinalResponseControlPlan(response);
    return result.control() == nullptr &&
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

RUVIA_TEST(final_response_control_entry_points_own_only_their_protocol) {
    HttpResponse http1(std::pmr::get_default_resource());
    http1.header("Connection", "close");
    http1.header(
        "Connection",
        "Upgrade",
        HttpResponse::HeaderOptions{.append = true});
    http1.header(
        "Connection",
        "X-Hop",
        HttpResponse::HeaderOptions{.append = true});
    http1.header("Upgrade", "websocket");
    http1.header("X-Hop", "value");

    const auto http1Result = http1FinalResponseControlPlan(http1);
    RUVIA_CHECK(http1Result.failure() == nullptr);
    const auto* http1Control = http1Result.control();
    RUVIA_CHECK(http1Control != nullptr);
    if (http1Control != nullptr) {
        RUVIA_CHECK(http1Control->connectionOptions().close());
        RUVIA_CHECK(http1Control->connectionOptions().upgrade());
        RUVIA_CHECK(http1Control->upgradeProtocols().hasField());
        RUVIA_CHECK(http1Control->upgradeProtocols().hasProtocol());
    }

    HttpResponse http2(std::pmr::get_default_resource());
    http2.header("X-Trace", "ok");
    const auto http2Result = http2FinalResponseControlPlan(http2);
    RUVIA_CHECK(http2Result.failure() == nullptr);
    RUVIA_CHECK(http2Result.control() != nullptr);
}

RUVIA_TEST(final_response_control_failure_never_exposes_protocol_alternative) {
    HttpResponse invalidConnection(std::pmr::get_default_resource());
    addUncheckedHeader(invalidConnection, "Connection", ", close");
    RUVIA_CHECK(isHttp1Failure(
        invalidConnection,
        Http1FinalResponseControlPlanError::kInvalidConnectionField));

    HttpResponse invalidUpgrade(std::pmr::get_default_resource());
    addUncheckedHeader(invalidUpgrade, "Upgrade", "web socket");
    RUVIA_CHECK(isHttp1Failure(
        invalidUpgrade,
        Http1FinalResponseControlPlanError::kInvalidUpgradeField));

    HttpResponse missingUpgrade(std::pmr::get_default_resource());
    missingUpgrade.status(ruvia::http_status::kUpgradeRequired);
    RUVIA_CHECK(isHttp1Failure(
        missingUpgrade,
        Http1FinalResponseControlPlanError::kUpgradeRequired));
    RUVIA_CHECK(isHttp2Failure(
        missingUpgrade,
        Http2FinalResponseControlPlanError::kUpgradeUnavailable));
}

RUVIA_TEST(final_response_control_rejects_end_to_end_connection_options) {
    for (const std::string_view option : {
             "content-length", "DATE", "Set-Cookie"}) {
        HttpResponse response(std::pmr::get_default_resource());
        response.header("Connection", option);
        RUVIA_CHECK(isHttp1Failure(
            response,
            Http1FinalResponseControlPlanError::kInvalidConnectionField));
    }
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
        RUVIA_CHECK(isHttp2Failure(
            response,
            Http2FinalResponseControlPlanError::
                kConnectionSpecificFieldForbidden));
    }
}
