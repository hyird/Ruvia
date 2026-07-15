#include "test_harness.h"

#include <concepts>
#include <string_view>

#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/web/detail/http/ContextServices.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/SecurityHeaders.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace {

using ruvia::Context;
using ruvia::HttpResponse;
using ruvia::LegacyXssFilterPolicy;
using ruvia::RequestMemory;
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
