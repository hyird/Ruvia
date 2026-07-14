#include "test_harness.h"

#include <concepts>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/HttpResponseContentSemantics.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::detail::HttpConnectTunnelResponseContent;
using ruvia::detail::HttpInformationalResponseContent;
using ruvia::detail::HttpProtocolSwitchResponseContent;
using ruvia::detail::HttpResponseContentSemantics;
using ruvia::detail::HttpResponseWithContent;
using ruvia::detail::HttpResponseWithoutContent;
using ruvia::detail::httpResponseContentSemantics;

template <typename T>
concept HasAnyRvalueResponseContentSemanticsAccessor =
    requires(T&& value) { std::move(value).informational(); } ||
    requires(T&& value) { std::move(value).protocolSwitch(); } ||
    requires(T&& value) { std::move(value).connectTunnel(); } ||
    requires(T&& value) { std::move(value).withoutContent(); } ||
    requires(T&& value) { std::move(value).withContent(); };

static_assert(!std::default_initializable<HttpResponseContentSemantics>);
static_assert(!HasAnyRvalueResponseContentSemanticsAccessor<
    HttpResponseContentSemantics>);
static_assert(!std::default_initializable<HttpInformationalResponseContent>);
static_assert(!std::default_initializable<HttpProtocolSwitchResponseContent>);
static_assert(!std::default_initializable<HttpConnectTunnelResponseContent>);
static_assert(!std::default_initializable<HttpResponseWithoutContent>);
static_assert(!std::default_initializable<HttpResponseWithContent>);

constexpr auto kHeadResponse = httpResponseContentSemantics(
    HttpKnownMethod::kHead, 200);
constexpr auto kConnectResponse = httpResponseContentSemantics(
    HttpKnownMethod::kConnect, 200);
static_assert(kHeadResponse.withoutContent() != nullptr);
static_assert(kConnectResponse.connectTunnel() != nullptr);

}  // namespace

RUVIA_TEST(response_content_semantics_owns_method_status_precedence) {
    const auto informational = httpResponseContentSemantics(
        HttpKnownMethod::kGet, 103);
    RUVIA_CHECK(informational.informational() != nullptr);
    RUVIA_CHECK(informational.protocolSwitch() == nullptr);

    const auto protocolSwitch = httpResponseContentSemantics(
        HttpKnownMethod::kGet, 101);
    RUVIA_CHECK(protocolSwitch.protocolSwitch() != nullptr);
    RUVIA_CHECK(protocolSwitch.informational() == nullptr);

    const auto tunnel = httpResponseContentSemantics(
        HttpKnownMethod::kConnect, 204);
    RUVIA_CHECK(tunnel.connectTunnel() != nullptr);
    RUVIA_CHECK(tunnel.withoutContent() == nullptr);

    for (const auto status : {204, 304}) {
        const auto withoutContent = httpResponseContentSemantics(
            HttpKnownMethod::kGet, status);
        RUVIA_CHECK(withoutContent.withoutContent() != nullptr);
        RUVIA_CHECK(withoutContent.withContent() == nullptr);
    }

    const auto head = httpResponseContentSemantics(
        HttpKnownMethod::kHead, 200);
    RUVIA_CHECK(head.withoutContent() != nullptr);

    const auto rejectedConnect = httpResponseContentSemantics(
        HttpKnownMethod::kConnect, 404);
    RUVIA_CHECK(rejectedConnect.withContent() != nullptr);

    // RFC 9110 Section 6.4.1 deliberately classifies 205 among responses that
    // have content (necessarily zero-length by Section 15.3.6), unlike 204.
    const auto resetContent = httpResponseContentSemantics(
        HttpKnownMethod::kGet, 205);
    RUVIA_CHECK(resetContent.withContent() != nullptr);
}

RUVIA_TEST(response_content_semantics_preserves_case_sensitive_method_tokens) {
    const auto head = httpResponseContentSemantics("HEAD", 200);
    const auto lowerHead = httpResponseContentSemantics("head", 200);
    const auto connect = httpResponseContentSemantics("CONNECT", 200);
    const auto lowerConnect = httpResponseContentSemantics("connect", 200);
    RUVIA_CHECK(head.withoutContent() != nullptr);
    RUVIA_CHECK(lowerHead.withContent() != nullptr);
    RUVIA_CHECK(connect.connectTunnel() != nullptr);
    RUVIA_CHECK(lowerConnect.withContent() != nullptr);
}
