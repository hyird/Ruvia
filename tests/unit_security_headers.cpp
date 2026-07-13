#include "test_harness.h"

#include <memory_resource>
#include <string_view>

#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/SecurityHeaders.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace {

using ruvia::Context;
using ruvia::HttpResponse;
using ruvia::RequestMemory;
using ruvia::SecurityHeadersOptions;
using ruvia::WorkerMemory;
using ruvia::applySecurityHeaders;
using ruvia::detail::ContextAccess;
using ruvia::detail::HttpRequestAccess;

HttpResponse makeResponse() {
    return HttpResponse(std::pmr::new_delete_resource());
}

Context makeContext(WorkerMemory& worker, RequestMemory& requestMemory) {
    auto request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    return ContextAccess::make(requestMemory, request);
}

}  // namespace

RUVIA_TEST(security_headers_default_set) {
    auto response = makeResponse();
    applySecurityHeaders(response, SecurityHeadersOptions{});

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

RUVIA_TEST(security_headers_empty_policy_is_not_emitted) {
    // An explicitly-cleared policy string produces no header (rather than an
    // empty-valued one).
    auto response = makeResponse();
    SecurityHeadersOptions options;
    options.contentSecurityPolicy = "";
    options.referrerPolicy = "";
    applySecurityHeaders(response, options);
    RUVIA_CHECK(response.header("Content-Security-Policy").empty());
    RUVIA_CHECK(response.header("Referrer-Policy").empty());
}

RUVIA_TEST(security_headers_disabled_options_omit_headers) {
    auto response = makeResponse();
    SecurityHeadersOptions options;
    options.frameOptions = false;
    options.strictTransportSecurity = false;
    applySecurityHeaders(response, options);

    RUVIA_CHECK(response.header("X-Frame-Options").empty());
    RUVIA_CHECK(response.header("Strict-Transport-Security").empty());
    RUVIA_CHECK_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
}

RUVIA_TEST(security_headers_emit_configured_policies) {
    auto response = makeResponse();
    SecurityHeadersOptions options;
    options.contentSecurityPolicy = "default-src 'self'";
    options.referrerPolicy = "no-referrer";
    applySecurityHeaders(response, options);

    RUVIA_CHECK_EQ(response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    RUVIA_CHECK_EQ(response.header("Referrer-Policy"), std::string_view("no-referrer"));
}

RUVIA_TEST(security_headers_apply_custom_headers) {
    auto response = makeResponse();
    const ruvia::SecurityHeader custom[] = {
        {"X-Custom-Security", "value-1"},
        {"X-Report-To", "endpoint"},
    };
    SecurityHeadersOptions options;
    options.customHeaders = custom;
    applySecurityHeaders(response, options);

    RUVIA_CHECK_EQ(response.header("X-Custom-Security"), std::string_view("value-1"));
    RUVIA_CHECK_EQ(response.header("X-Report-To"), std::string_view("endpoint"));
    // Built-in defaults are still applied alongside custom headers.
    RUVIA_CHECK_EQ(response.header("X-Frame-Options"), std::string_view("DENY"));
}

RUVIA_TEST(security_headers_respect_overwrite_existing_flag) {
    // The default (overwriteExisting = false) must not clobber a header a handler
    // already set -- the middleware only supplies defaults.
    auto keep = makeResponse();
    keep.header("X-Frame-Options", "SAMEORIGIN");
    applySecurityHeaders(keep, SecurityHeadersOptions{});
    RUVIA_CHECK_EQ(keep.header("X-Frame-Options"), std::string_view("SAMEORIGIN"));

    // With overwriteExisting = true, the security default replaces it.
    auto replace = makeResponse();
    replace.header("X-Frame-Options", "SAMEORIGIN");
    SecurityHeadersOptions overwrite;
    overwrite.overwriteExisting = true;
    applySecurityHeaders(replace, overwrite);
    RUVIA_CHECK_EQ(replace.header("X-Frame-Options"), std::string_view("DENY"));
}

RUVIA_TEST(security_headers_context_respects_overwrite_existing_flag) {
    WorkerMemory worker;
    RequestMemory keepMemory(worker);
    auto keep = makeContext(worker, keepMemory);
    keep.header("X-Frame-Options", "SAMEORIGIN");
    applySecurityHeaders(keep, SecurityHeadersOptions{});
    RUVIA_CHECK(keep.response() == nullptr);
    RUVIA_CHECK_EQ(
        ruvia::detail::ContextAccess::responseStorage(keep).header("X-Frame-Options"),
        std::string_view("SAMEORIGIN"));

    RequestMemory replaceMemory(worker);
    auto replace = makeContext(worker, replaceMemory);
    replace.header("X-Frame-Options", "SAMEORIGIN");
    SecurityHeadersOptions overwrite;
    overwrite.overwriteExisting = true;
    applySecurityHeaders(replace, overwrite);
    RUVIA_CHECK(replace.response() == nullptr);
    RUVIA_CHECK_EQ(
        ruvia::detail::ContextAccess::responseStorage(replace).header("X-Frame-Options"),
        std::string_view("DENY"));
}
