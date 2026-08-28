#include "test_harness.h"

#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
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

using ruvia::applySecurityHeaders;
using ruvia::Context;
using ruvia::DefaultSecurityHeaderPolicy;
using ruvia::HttpResponse;
using ruvia::RequestMemory;
using ruvia::SecurityHeader;
using ruvia::SecurityHeaderConflictPolicy;
using ruvia::SecurityHeadersConfig;
using ruvia::WorkerMemory;
using ruvia::XssProtectionHeaderPolicy;
using ruvia::detail::ContextAccess;
using ruvia::detail::ContextServices;
using ruvia::detail::HttpRequestAccess;

template <typename Target>
concept HasContextlessSecurityHeaders = requires(Target& target, const SecurityHeadersConfig& options) { ruvia::applySecurityHeaders(target, options); };

static_assert(!HasContextlessSecurityHeaders<HttpResponse>);

template <typename T>
concept HasSecurityHeadersOverwriteExistingBoolean = requires(T& options) { options.overwriteExisting = true; };

static_assert(std::is_aggregate_v<SecurityHeader>);
static_assert(std::is_aggregate_v<SecurityHeadersConfig>);
static_assert(std::same_as<decltype(SecurityHeader{}.name), std::string>);
static_assert(std::same_as<decltype(SecurityHeader{}.value), std::string>);
static_assert(std::same_as<decltype(SecurityHeadersConfig{}.contentSecurityPolicy), std::string>);
static_assert(std::same_as<decltype(SecurityHeadersConfig{}.referrerPolicy), std::string>);
static_assert(std::same_as<decltype(SecurityHeadersConfig{}.permissionsPolicy), std::string>);
static_assert(std::same_as<decltype(SecurityHeadersConfig{}.customHeaders), std::vector<SecurityHeader>>);
static_assert(SecurityHeadersConfig{}.contentTypeOptionsHeader == DefaultSecurityHeaderPolicy::kEmitDefault);
static_assert(SecurityHeadersConfig{}.frameOptionsHeader == DefaultSecurityHeaderPolicy::kEmitDefault);
static_assert(SecurityHeadersConfig{}.strictTransportSecurityHeader == DefaultSecurityHeaderPolicy::kEmitDefault);
static_assert(std::same_as<decltype(SecurityHeadersConfig{}.existingHeaders), SecurityHeaderConflictPolicy>);
static_assert(SecurityHeadersConfig{}.existingHeaders == SecurityHeaderConflictPolicy::kPreserveExisting);
static_assert(!HasSecurityHeadersOverwriteExistingBoolean<SecurityHeadersConfig>);
static_assert(!std::is_copy_constructible_v<ruvia::SecurityHeadersMiddleware>);
static_assert(!std::is_copy_assignable_v<ruvia::SecurityHeadersMiddleware>);
static_assert(!std::is_move_constructible_v<ruvia::SecurityHeadersMiddleware>);
static_assert(!std::is_move_assignable_v<ruvia::SecurityHeadersMiddleware>);

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
    SecurityContextFixture fixture(ContextServices{}.withTlsTransport("192.0.2.1"));
    applySecurityHeaders(fixture.context(), SecurityHeadersConfig{});
    const auto& response = fixture.response();

    RUVIA_CHECK_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
    RUVIA_CHECK_EQ(response.header("X-Frame-Options"), std::string_view("DENY"));
    RUVIA_CHECK_EQ(response.header("Strict-Transport-Security"), std::string_view("max-age=31536000; includeSubDomains"));
    // Modern guidance disables the legacy XSS auditor rather than enabling it.
    RUVIA_CHECK_EQ(response.header("X-XSS-Protection"), std::string_view("0"));
    // Secure-by-default policy values ship out of the box.
    RUVIA_CHECK_EQ(response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    RUVIA_CHECK_EQ(response.header("Referrer-Policy"), std::string_view("strict-origin-when-cross-origin"));
    RUVIA_CHECK_EQ(response.header("Permissions-Policy"), std::string_view("geolocation=(), microphone=(), camera=()"));
}

RUVIA_TEST(security_headers_emit_hsts_only_for_tls_contexts) {
    SecurityContextFixture plain(ContextServices{}.withPlainTransport("192.0.2.1"));
    applySecurityHeaders(plain.context());
    RUVIA_CHECK(!plain.response().header("Strict-Transport-Security").has_value());

    SecurityContextFixture tls(ContextServices{}.withTlsTransport("192.0.2.2"));
    applySecurityHeaders(tls.context());
    RUVIA_CHECK_EQ(tls.response().header("Strict-Transport-Security"), std::string_view("max-age=31536000; includeSubDomains"));
}

RUVIA_TEST(security_headers_xss_protection_header_policy_is_explicit) {
    static_assert(SecurityHeadersConfig{}.xssProtectionHeader == XssProtectionHeaderPolicy::kEmitDisabled);

    SecurityContextFixture fixture;
    const SecurityHeadersConfig options{
        .xssProtectionHeader = XssProtectionHeaderPolicy::kOmit,
    };
    applySecurityHeaders(fixture.context(), options);

    RUVIA_CHECK(!fixture.response().header("X-XSS-Protection").has_value());
}

RUVIA_TEST(security_headers_reject_invalid_xss_protection_header_policy) {
    SecurityContextFixture fixture;
    const SecurityHeadersConfig options{
        .xssProtectionHeader = std::bit_cast<XssProtectionHeaderPolicy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        applySecurityHeaders(fixture.context(), options);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(security_headers_reject_invalid_default_header_policy) {
    SecurityContextFixture fixture;
    const SecurityHeadersConfig options{
        .frameOptionsHeader = std::bit_cast<DefaultSecurityHeaderPolicy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        applySecurityHeaders(fixture.context(), options);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(security_headers_empty_policy_is_not_emitted) {
    // An explicitly-cleared policy string produces no header (rather than an
    // empty-valued one).
    SecurityContextFixture fixture;
    SecurityHeadersConfig options;
    options.contentSecurityPolicy = "";
    options.referrerPolicy = "";
    applySecurityHeaders(fixture.context(), options);
    const auto& response = fixture.response();
    RUVIA_CHECK(!response.header("Content-Security-Policy").has_value());
    RUVIA_CHECK(!response.header("Referrer-Policy").has_value());
}

RUVIA_TEST(security_headers_disabled_options_omit_headers) {
    SecurityContextFixture fixture;
    SecurityHeadersConfig options;
    options.frameOptionsHeader = DefaultSecurityHeaderPolicy::kOmit;
    options.strictTransportSecurityHeader = DefaultSecurityHeaderPolicy::kOmit;
    applySecurityHeaders(fixture.context(), options);
    const auto& response = fixture.response();

    RUVIA_CHECK(!response.header("X-Frame-Options").has_value());
    RUVIA_CHECK(!response.header("Strict-Transport-Security").has_value());
    RUVIA_CHECK_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
}

RUVIA_TEST(security_headers_emit_configured_policies) {
    SecurityContextFixture fixture;
    SecurityHeadersConfig options;
    options.contentSecurityPolicy = "default-src 'self'";
    options.referrerPolicy = "no-referrer";
    applySecurityHeaders(fixture.context(), options);
    const auto& response = fixture.response();

    RUVIA_CHECK_EQ(response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    RUVIA_CHECK_EQ(response.header("Referrer-Policy"), std::string_view("no-referrer"));
}

RUVIA_TEST(security_headers_apply_custom_headers) {
    SecurityContextFixture fixture;
    const std::vector<ruvia::SecurityHeader> custom = {
        {"X-Custom-Security", "value-1"},
        {"X-Report-To", "endpoint"},
    };
    SecurityHeadersConfig options;
    options.customHeaders = custom;
    applySecurityHeaders(fixture.context(), options);
    const auto& response = fixture.response();

    RUVIA_CHECK_EQ(response.header("X-Custom-Security"), std::string_view("value-1"));
    RUVIA_CHECK_EQ(response.header("X-Report-To"), std::string_view("endpoint"));
    // Built-in defaults are still applied alongside custom headers.
    RUVIA_CHECK_EQ(response.header("X-Frame-Options"), std::string_view("DENY"));
}

RUVIA_TEST(security_headers_reject_invalid_custom_header_at_construction) {
    bool invalidNameRejected = false;
    try {
        static_cast<void>(ruvia::SecurityHeadersMiddleware(SecurityHeadersConfig{
            .customHeaders = {{"bad name", "value"}},
        }));
    } catch (const std::invalid_argument&) {
        invalidNameRejected = true;
    }
    RUVIA_CHECK(invalidNameRejected);

    bool invalidValueRejected = false;
    try {
        static_cast<void>(ruvia::SecurityHeadersMiddleware(SecurityHeadersConfig{
            .customHeaders = {{"X-Security", "bad\r\nvalue"}},
        }));
    } catch (const std::invalid_argument&) {
        invalidValueRejected = true;
    }
    RUVIA_CHECK(invalidValueRejected);
}

RUVIA_TEST(security_headers_custom_hsts_is_still_tls_only) {
    const std::vector<ruvia::SecurityHeader> custom = {
        {"Strict-Transport-Security", "max-age=1"},
    };
    SecurityHeadersConfig options;
    options.strictTransportSecurityHeader = DefaultSecurityHeaderPolicy::kOmit;
    options.customHeaders = custom;

    SecurityContextFixture plain(ContextServices{}.withPlainTransport("192.0.2.1"));
    applySecurityHeaders(plain.context(), options);
    RUVIA_CHECK(!plain.response().header("Strict-Transport-Security").has_value());

    SecurityContextFixture tls(ContextServices{}.withTlsTransport("192.0.2.2"));
    applySecurityHeaders(tls.context(), options);
    RUVIA_CHECK_EQ(tls.response().header("Strict-Transport-Security"), std::string_view("max-age=1"));
}

RUVIA_TEST(security_headers_respect_existing_header_conflict_policy) {
    // The default preserves a header a handler already set -- the middleware
    // only supplies defaults.
    SecurityContextFixture keep;
    keep.context().header("X-Frame-Options", "SAMEORIGIN");
    applySecurityHeaders(keep.context(), SecurityHeadersConfig{});
    RUVIA_CHECK_EQ(keep.response().header("X-Frame-Options"), std::string_view("SAMEORIGIN"));

    SecurityContextFixture replace;
    replace.context().header("X-Frame-Options", "SAMEORIGIN");
    applySecurityHeaders(replace.context(), SecurityHeadersConfig{
                                                .existingHeaders = SecurityHeaderConflictPolicy::kReplaceExisting,
                                            });
    RUVIA_CHECK_EQ(replace.response().header("X-Frame-Options"), std::string_view("DENY"));
}

RUVIA_TEST(security_headers_reject_invalid_conflict_policy) {
    SecurityContextFixture fixture;
    const SecurityHeadersConfig options{
        .existingHeaders = std::bit_cast<SecurityHeaderConflictPolicy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        applySecurityHeaders(fixture.context(), options);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
