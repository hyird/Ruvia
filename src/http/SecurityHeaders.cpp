#include "ruvia/http/SecurityHeaders.h"

namespace ruvia {
namespace {

void setContextHeaderIfEnabled(Context& context, std::string_view name, std::string_view value) {
    if (!value.empty()) {
        context.header(name, value);
    }
}

void setResponseHeaderIfEnabled(
    HttpResponse& response,
    std::string_view name,
    std::string_view value,
    bool overwriteExisting) {
    if (value.empty()) {
        return;
    }
    if (!overwriteExisting && !response.header(name).empty()) {
        return;
    }
    response.setHeader(name, value);
}

void setResponseHeader(
    HttpResponse& response,
    std::string_view name,
    std::string_view value,
    bool overwriteExisting) {
    if (!overwriteExisting && !response.header(name).empty()) {
        return;
    }
    response.setHeader(name, value);
}

}  // namespace

void applySecurityHeaders(Context& context, const SecurityHeadersOptions& options) {
    if (options.contentTypeOptions) {
        context.header("X-Content-Type-Options", "nosniff");
    }
    if (options.frameOptions) {
        context.header("X-Frame-Options", "DENY");
    }
    if (options.strictTransportSecurity) {
        context.header("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
    }
    if (options.xssProtection) {
        context.header("X-XSS-Protection", "0");
    }

    setContextHeaderIfEnabled(context, "Content-Security-Policy", options.contentSecurityPolicy);
    setContextHeaderIfEnabled(context, "Referrer-Policy", options.referrerPolicy);
    setContextHeaderIfEnabled(context, "Permissions-Policy", options.permissionsPolicy);

    for (const auto& header : options.customHeaders) {
        context.header(header.name, header.value);
    }
}

void applySecurityHeaders(HttpResponse& response, const SecurityHeadersOptions& options) {
    if (options.contentTypeOptions) {
        setResponseHeaderIfEnabled(
            response,
            "X-Content-Type-Options",
            "nosniff",
            options.overwriteExisting);
    }
    if (options.frameOptions) {
        setResponseHeaderIfEnabled(response, "X-Frame-Options", "DENY", options.overwriteExisting);
    }
    if (options.strictTransportSecurity) {
        setResponseHeaderIfEnabled(
            response,
            "Strict-Transport-Security",
            "max-age=31536000; includeSubDomains",
            options.overwriteExisting);
    }
    if (options.xssProtection) {
        setResponseHeaderIfEnabled(response, "X-XSS-Protection", "0", options.overwriteExisting);
    }

    setResponseHeaderIfEnabled(
        response,
        "Content-Security-Policy",
        options.contentSecurityPolicy,
        options.overwriteExisting);
    setResponseHeaderIfEnabled(response, "Referrer-Policy", options.referrerPolicy, options.overwriteExisting);
    setResponseHeaderIfEnabled(response, "Permissions-Policy", options.permissionsPolicy, options.overwriteExisting);

    for (const auto& header : options.customHeaders) {
        setResponseHeader(response, header.name, header.value, options.overwriteExisting);
    }
}

Task<void> SecurityHeadersMiddleware::handle(Context& context, const Next& next) {
    applySecurityHeaders(context);
    co_await next();
    applySecurityHeaders(context.res());
}

}  // namespace ruvia
