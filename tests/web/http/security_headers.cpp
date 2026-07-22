#include "test_harness.h"

#include <array>
#include <concepts>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/SecurityHeaders.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace {

using ruvia::Context;
using ruvia::HttpResponse;
using ruvia::LegacyXssFilterPolicy;
using ruvia::RequestMemory;
using ruvia::SecurityHeader;
using ruvia::SecurityHeadersOptions;
using ruvia::WorkerMemory;
using ruvia::applySecurityHeaders;
using ruvia::detail::ContextAccess;
using ruvia::detail::ContextServices;
using ruvia::detail::HttpRequestAccess;

template <typename Target>
concept HasContextlessSecurityHeaders = requires(
    Target& target,
    const SecurityHeadersOptions& options) {
    ruvia::applySecurityHeaders(target, options);
};

static_assert(!HasContextlessSecurityHeaders<HttpResponse>);

template <typename Text>
concept AcceptsAnySecurityHeaderText =
    requires(Text&& text) {
        SecurityHeader{
            .name = std::forward<Text>(text),
            .value = "value",
        };
    } ||
    requires(Text&& text) {
        SecurityHeader{
            .name = "X-Test",
            .value = std::forward<Text>(text),
        };
    };

template <typename Text>
concept AcceptsAllSecurityHeaderText = requires(Text&& text) {
    SecurityHeader{
        .name = std::forward<Text>(text),
        .value = "value",
    };
    SecurityHeader{
        .name = "X-Test",
        .value = std::forward<Text>(text),
    };
};

template <typename Text>
concept AssignsAnySecurityHeaderText =
    requires(SecurityHeader& header, Text&& text) {
        header.name = std::forward<Text>(text);
    } ||
    requires(SecurityHeader& header, Text&& text) {
        header.value = std::forward<Text>(text);
    };

template <typename Text>
concept AssignsAllSecurityHeaderText =
    requires(SecurityHeader& header, Text&& text) {
        header.name = std::forward<Text>(text);
        header.value = std::forward<Text>(text);
    };

template <typename Text>
concept AcceptsAnySecurityPolicyText =
    requires(Text&& text) {
        SecurityHeadersOptions{
            .contentSecurityPolicy = std::forward<Text>(text),
        };
    } ||
    requires(Text&& text) {
        SecurityHeadersOptions{
            .referrerPolicy = std::forward<Text>(text),
        };
    } ||
    requires(Text&& text) {
        SecurityHeadersOptions{
            .permissionsPolicy = std::forward<Text>(text),
        };
    };

template <typename Text>
concept AcceptsAllSecurityPolicyText = requires(Text&& text) {
    SecurityHeadersOptions{
        .contentSecurityPolicy = std::forward<Text>(text),
    };
    SecurityHeadersOptions{
        .referrerPolicy = std::forward<Text>(text),
    };
    SecurityHeadersOptions{
        .permissionsPolicy = std::forward<Text>(text),
    };
};

template <typename Text>
concept AssignsAnySecurityPolicyText =
    requires(SecurityHeadersOptions& options, Text&& text) {
        options.contentSecurityPolicy = std::forward<Text>(text);
    } ||
    requires(SecurityHeadersOptions& options, Text&& text) {
        options.referrerPolicy = std::forward<Text>(text);
    } ||
    requires(SecurityHeadersOptions& options, Text&& text) {
        options.permissionsPolicy = std::forward<Text>(text);
    };

template <typename Text>
concept AssignsAllSecurityPolicyText =
    requires(SecurityHeadersOptions& options, Text&& text) {
        options.contentSecurityPolicy = std::forward<Text>(text);
        options.referrerPolicy = std::forward<Text>(text);
        options.permissionsPolicy = std::forward<Text>(text);
    };

template <typename Headers>
concept AcceptsSecurityCustomHeaders = requires(Headers&& headers) {
    SecurityHeadersOptions{
        .customHeaders = std::forward<Headers>(headers),
    };
};

template <typename Headers>
concept AssignsSecurityCustomHeaders = requires(
    SecurityHeadersOptions& options,
    Headers&& headers) {
    options.customHeaders = std::forward<Headers>(headers);
};

using SecurityHeaderArray = std::array<SecurityHeader, 1>;
using SecurityHeaderVector = std::vector<SecurityHeader>;

static_assert(std::is_aggregate_v<SecurityHeader>);
static_assert(std::is_aggregate_v<SecurityHeadersOptions>);
constexpr SecurityHeader kLiteralSecurityHeader{
    .name = "X-Test",
    .value = "value",
};
constexpr std::array kLiteralSecurityHeaders{kLiteralSecurityHeader};
constexpr SecurityHeadersOptions kLiteralSecurityHeaderOptions{
    .contentSecurityPolicy = "default-src 'none'",
    .customHeaders = kLiteralSecurityHeaders,
};
static_assert(kLiteralSecurityHeader.name.view() == "X-Test");
static_assert(kLiteralSecurityHeader.value.view() == "value");
static_assert(
    kLiteralSecurityHeaderOptions.contentSecurityPolicy.view() ==
    "default-src 'none'");
static_assert(kLiteralSecurityHeaderOptions.customHeaders.size() == 1);
static_assert(!AcceptsAnySecurityHeaderText<std::string>);
static_assert(!AcceptsAnySecurityHeaderText<const std::string>);
static_assert(!AcceptsAnySecurityHeaderText<std::pmr::string>);
static_assert(AcceptsAllSecurityHeaderText<std::string&>);
static_assert(AcceptsAllSecurityHeaderText<std::pmr::string&>);
static_assert(AcceptsAllSecurityHeaderText<std::string_view>);
static_assert(!AssignsAnySecurityHeaderText<std::string>);
static_assert(!AssignsAnySecurityHeaderText<const std::string>);
static_assert(!AssignsAnySecurityHeaderText<std::pmr::string>);
static_assert(AssignsAllSecurityHeaderText<std::string&>);
static_assert(AssignsAllSecurityHeaderText<std::pmr::string&>);
static_assert(AssignsAllSecurityHeaderText<std::string_view>);
static_assert(!AcceptsAnySecurityPolicyText<std::string>);
static_assert(!AcceptsAnySecurityPolicyText<const std::string>);
static_assert(!AcceptsAnySecurityPolicyText<std::pmr::string>);
static_assert(AcceptsAllSecurityPolicyText<std::string&>);
static_assert(AcceptsAllSecurityPolicyText<std::pmr::string&>);
static_assert(AcceptsAllSecurityPolicyText<std::string_view>);
static_assert(!AssignsAnySecurityPolicyText<std::string>);
static_assert(!AssignsAnySecurityPolicyText<const std::string>);
static_assert(!AssignsAnySecurityPolicyText<std::pmr::string>);
static_assert(AssignsAllSecurityPolicyText<std::string&>);
static_assert(AssignsAllSecurityPolicyText<std::pmr::string&>);
static_assert(AssignsAllSecurityPolicyText<std::string_view>);
static_assert(!AcceptsSecurityCustomHeaders<SecurityHeaderArray>);
static_assert(!AcceptsSecurityCustomHeaders<const SecurityHeaderArray>);
static_assert(!AcceptsSecurityCustomHeaders<SecurityHeaderVector>);
static_assert(!AcceptsSecurityCustomHeaders<const SecurityHeaderVector>);
static_assert(AcceptsSecurityCustomHeaders<SecurityHeaderArray&>);
static_assert(AcceptsSecurityCustomHeaders<SecurityHeaderVector&>);
static_assert(AcceptsSecurityCustomHeaders<std::span<const SecurityHeader>>);
static_assert(!AssignsSecurityCustomHeaders<SecurityHeaderArray>);
static_assert(!AssignsSecurityCustomHeaders<const SecurityHeaderArray>);
static_assert(!AssignsSecurityCustomHeaders<SecurityHeaderVector>);
static_assert(!AssignsSecurityCustomHeaders<const SecurityHeaderVector>);
static_assert(AssignsSecurityCustomHeaders<SecurityHeaderArray&>);
static_assert(AssignsSecurityCustomHeaders<SecurityHeaderVector&>);
static_assert(AssignsSecurityCustomHeaders<std::span<const SecurityHeader>>);

class SecurityContextFixture final {
public:
    explicit SecurityContextFixture(ContextServices services = {})
        : memory_(worker_),
          request_(makeRequest(memory_)),
          context_(ContextAccess::make(memory_, request_, services)) {}

    [[nodiscard]] Context& context() noexcept {
        return context_;
    }

    [[nodiscard]] HttpResponse& response() {
        return ContextAccess::responseStorage(context_);
    }

private:
    [[nodiscard]] static ruvia::HttpRequest makeRequest(RequestMemory& memory) {
        auto request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setResource(request, memory.resource());
        return request;
    }

    WorkerMemory worker_;
    RequestMemory memory_;
    ruvia::HttpRequest request_;
    Context context_;
};

}  // namespace

RUVIA_TEST(security_headers_default_set) {
    SecurityContextFixture fixture(
        ContextServices{}.withTlsTransport("192.0.2.1"));
    applySecurityHeaders(fixture.context(), SecurityHeadersOptions{});
    const auto& response = fixture.response();

    RUVIA_CHECK_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
    RUVIA_CHECK_EQ(response.header("X-Frame-Options"), std::string_view("DENY"));
    RUVIA_CHECK_EQ(response.header("Strict-Transport-Security"),
                   std::string_view("max-age=31536000; includeSubDomains"));
    // Modern guidance disables the legacy XSS auditor rather than enabling it.
    RUVIA_CHECK_EQ(response.header("X-XSS-Protection"), std::string_view("0"));
    // Secure-by-default policy values ship out of the box.
    RUVIA_CHECK_EQ(response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    RUVIA_CHECK_EQ(response.header("Referrer-Policy"),
                   std::string_view("strict-origin-when-cross-origin"));
    RUVIA_CHECK_EQ(response.header("Permissions-Policy"),
                   std::string_view("geolocation=(), microphone=(), camera=()"));
}

RUVIA_TEST(security_headers_emit_hsts_only_for_tls_contexts) {
    SecurityContextFixture plain(
        ContextServices{}.withPlainTransport("192.0.2.1"));
    applySecurityHeaders(plain.context());
    RUVIA_CHECK(
        !plain.response().header("Strict-Transport-Security").has_value());

    SecurityContextFixture tls(
        ContextServices{}.withTlsTransport("192.0.2.2"));
    applySecurityHeaders(tls.context());
    RUVIA_CHECK_EQ(
        tls.response().header("Strict-Transport-Security"),
        std::string_view("max-age=31536000; includeSubDomains"));
}

RUVIA_TEST(security_headers_legacy_xss_filter_policy_is_explicit) {
    static_assert(
        SecurityHeadersOptions{}.legacyXssFilter ==
        LegacyXssFilterPolicy::kDisable);

    SecurityContextFixture fixture;
    SecurityHeadersOptions options;
    options.legacyXssFilter = LegacyXssFilterPolicy::kOmitHeader;
    applySecurityHeaders(fixture.context(), options);

    RUVIA_CHECK(!fixture.response().header("X-XSS-Protection").has_value());
}

RUVIA_TEST(security_headers_empty_policy_is_not_emitted) {
    // An explicitly-cleared policy string produces no header (rather than an
    // empty-valued one).
    SecurityContextFixture fixture;
    SecurityHeadersOptions options;
    options.contentSecurityPolicy = "";
    options.referrerPolicy = "";
    applySecurityHeaders(fixture.context(), options);
    const auto& response = fixture.response();
    RUVIA_CHECK(!response.header("Content-Security-Policy").has_value());
    RUVIA_CHECK(!response.header("Referrer-Policy").has_value());
}

RUVIA_TEST(security_headers_disabled_options_omit_headers) {
    SecurityContextFixture fixture;
    SecurityHeadersOptions options;
    options.frameOptions = false;
    options.strictTransportSecurity = false;
    applySecurityHeaders(fixture.context(), options);
    const auto& response = fixture.response();

    RUVIA_CHECK(!response.header("X-Frame-Options").has_value());
    RUVIA_CHECK(!response.header("Strict-Transport-Security").has_value());
    RUVIA_CHECK_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
}

RUVIA_TEST(security_headers_emit_configured_policies) {
    SecurityContextFixture fixture;
    SecurityHeadersOptions options;
    options.contentSecurityPolicy = "default-src 'self'";
    options.referrerPolicy = "no-referrer";
    applySecurityHeaders(fixture.context(), options);
    const auto& response = fixture.response();

    RUVIA_CHECK_EQ(response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    RUVIA_CHECK_EQ(response.header("Referrer-Policy"), std::string_view("no-referrer"));
}

RUVIA_TEST(security_headers_apply_custom_headers) {
    SecurityContextFixture fixture;
    const ruvia::SecurityHeader custom[] = {
        {"X-Custom-Security", "value-1"},
        {"X-Report-To", "endpoint"},
    };
    SecurityHeadersOptions options;
    options.customHeaders = custom;
    applySecurityHeaders(fixture.context(), options);
    const auto& response = fixture.response();

    RUVIA_CHECK_EQ(response.header("X-Custom-Security"), std::string_view("value-1"));
    RUVIA_CHECK_EQ(response.header("X-Report-To"), std::string_view("endpoint"));
    // Built-in defaults are still applied alongside custom headers.
    RUVIA_CHECK_EQ(response.header("X-Frame-Options"), std::string_view("DENY"));
}

RUVIA_TEST(security_headers_respect_overwrite_existing_flag) {
    // The default (overwriteExisting = false) must not clobber a header a handler
    // already set -- the middleware only supplies defaults.
    SecurityContextFixture keep;
    keep.context().header("X-Frame-Options", "SAMEORIGIN");
    applySecurityHeaders(keep.context(), SecurityHeadersOptions{});
    RUVIA_CHECK_EQ(
        keep.response().header("X-Frame-Options"),
        std::string_view("SAMEORIGIN"));

    // With overwriteExisting = true, the security default replaces it.
    SecurityContextFixture replace;
    replace.context().header("X-Frame-Options", "SAMEORIGIN");
    SecurityHeadersOptions overwrite;
    overwrite.overwriteExisting = true;
    applySecurityHeaders(replace.context(), overwrite);
    RUVIA_CHECK_EQ(
        replace.response().header("X-Frame-Options"),
        std::string_view("DENY"));
}
