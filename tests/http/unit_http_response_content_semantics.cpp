#include "test_harness.h"

#include <type_traits>

#include "ruvia/http/detail/HttpResponseContentSemantics.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::detail::HttpResponseContentSemantics;
using ruvia::detail::httpResponseContentSemantics;

static_assert(std::is_enum_v<HttpResponseContentSemantics>);
static_assert(sizeof(HttpResponseContentSemantics) == 1);

constexpr auto kHeadResponse = httpResponseContentSemantics(
    HttpKnownMethod::kHead, 200);
constexpr auto kConnectResponse = httpResponseContentSemantics(
    HttpKnownMethod::kConnect, 200);
static_assert(
    kHeadResponse == HttpResponseContentSemantics::kWithoutContent);
static_assert(
    kConnectResponse == HttpResponseContentSemantics::kConnectTunnel);

}  // namespace

RUVIA_TEST(response_content_semantics_owns_method_status_precedence) {
    const auto informational = httpResponseContentSemantics(
        HttpKnownMethod::kGet, 103);
    RUVIA_CHECK(
        informational == HttpResponseContentSemantics::kInformational);

    const auto protocolSwitch = httpResponseContentSemantics(
        HttpKnownMethod::kGet, 101);
    RUVIA_CHECK(
        protocolSwitch == HttpResponseContentSemantics::kProtocolSwitch);

    const auto tunnel = httpResponseContentSemantics(
        HttpKnownMethod::kConnect, 204);
    RUVIA_CHECK(tunnel == HttpResponseContentSemantics::kConnectTunnel);

    for (const auto status : {204, 304}) {
        const auto withoutContent = httpResponseContentSemantics(
            HttpKnownMethod::kGet, status);
        RUVIA_CHECK(
            withoutContent == HttpResponseContentSemantics::kWithoutContent);
    }

    const auto head = httpResponseContentSemantics(
        HttpKnownMethod::kHead, 200);
    RUVIA_CHECK(head == HttpResponseContentSemantics::kWithoutContent);

    const auto rejectedConnect = httpResponseContentSemantics(
        HttpKnownMethod::kConnect, 404);
    RUVIA_CHECK(rejectedConnect == HttpResponseContentSemantics::kWithContent);

    // RFC 9110 Section 6.4.1 deliberately classifies 205 among responses that
    // have content (necessarily zero-length by Section 15.3.6), unlike 204.
    const auto resetContent = httpResponseContentSemantics(
        HttpKnownMethod::kGet, 205);
    RUVIA_CHECK(resetContent == HttpResponseContentSemantics::kWithContent);
}

RUVIA_TEST(response_content_semantics_preserves_case_sensitive_method_tokens) {
    const auto head = httpResponseContentSemantics("HEAD", 200);
    const auto lowerHead = httpResponseContentSemantics("head", 200);
    const auto connect = httpResponseContentSemantics("CONNECT", 200);
    const auto lowerConnect = httpResponseContentSemantics("connect", 200);
    RUVIA_CHECK(head == HttpResponseContentSemantics::kWithoutContent);
    RUVIA_CHECK(lowerHead == HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK(connect == HttpResponseContentSemantics::kConnectTunnel);
    RUVIA_CHECK(lowerConnect == HttpResponseContentSemantics::kWithContent);
}
