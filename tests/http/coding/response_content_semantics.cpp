#include "test_harness.h"

#include <type_traits>

#include "ruvia/http/detail/coding/HttpResponseContentSemantics.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::detail::HttpResponseContentSemantics;
using ruvia::detail::httpResponseContentSemantics;

static_assert(std::is_enum_v<HttpResponseContentSemantics>);
static_assert(sizeof(HttpResponseContentSemantics) == 1);

constexpr auto kHeadResponse = httpResponseContentSemantics(HttpKnownMethod::kHead, ruvia::http_status::kOk);
constexpr auto kConnectResponse = httpResponseContentSemantics(HttpKnownMethod::kConnect, ruvia::http_status::kOk);
static_assert(kHeadResponse == HttpResponseContentSemantics::kWithoutContent);
static_assert(kConnectResponse == HttpResponseContentSemantics::kConnectTunnel);

}  // namespace

RUVIA_TEST(response_content_semantics_owns_method_status_precedence) {
    const auto informational = httpResponseContentSemantics(HttpKnownMethod::kGet, ruvia::http_status::kEarlyHints);
    RUVIA_CHECK(informational == HttpResponseContentSemantics::kInformational);

    const auto protocolSwitch = httpResponseContentSemantics(HttpKnownMethod::kGet, ruvia::http_status::kSwitchingProtocols);
    RUVIA_CHECK(protocolSwitch == HttpResponseContentSemantics::kProtocolSwitch);

    const auto tunnel = httpResponseContentSemantics(HttpKnownMethod::kConnect, ruvia::http_status::kNoContent);
    RUVIA_CHECK(tunnel == HttpResponseContentSemantics::kConnectTunnel);

    for (const ruvia::HttpStatusCode status : {ruvia::http_status::kNoContent, ruvia::http_status::kNotModified}) {
        const auto withoutContent = httpResponseContentSemantics(HttpKnownMethod::kGet, status);
        RUVIA_CHECK(withoutContent == HttpResponseContentSemantics::kWithoutContent);
    }

    const auto head = httpResponseContentSemantics(HttpKnownMethod::kHead, ruvia::http_status::kOk);
    RUVIA_CHECK(head == HttpResponseContentSemantics::kWithoutContent);

    const auto rejectedConnect = httpResponseContentSemantics(HttpKnownMethod::kConnect, ruvia::http_status::kNotFound);
    RUVIA_CHECK(rejectedConnect == HttpResponseContentSemantics::kWithContent);

    // RFC 9110 Section 6.4.1 deliberately classifies 205 among responses that
    // have content (necessarily zero-length by Section 15.3.6), unlike 204.
    const auto resetContent = httpResponseContentSemantics(HttpKnownMethod::kGet, ruvia::http_status::kResetContent);
    RUVIA_CHECK(resetContent == HttpResponseContentSemantics::kWithContent);
}

RUVIA_TEST(response_content_semantics_preserves_case_sensitive_method_tokens) {
    const auto head = httpResponseContentSemantics("HEAD", ruvia::http_status::kOk);
    const auto lowerHead = httpResponseContentSemantics("head", ruvia::http_status::kOk);
    const auto connect = httpResponseContentSemantics("CONNECT", ruvia::http_status::kOk);
    const auto lowerConnect = httpResponseContentSemantics("connect", ruvia::http_status::kOk);
    RUVIA_CHECK(head == HttpResponseContentSemantics::kWithoutContent);
    RUVIA_CHECK(lowerHead == HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK(connect == HttpResponseContentSemantics::kConnectTunnel);
    RUVIA_CHECK(lowerConnect == HttpResponseContentSemantics::kWithContent);
}
