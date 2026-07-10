#include "ruvia/web/SecurityHeaders.h"

namespace ruvia {
namespace {

[[nodiscard]] bool hasSecurityHeader(Context& context, std::string_view name) noexcept {
    return !context.res().header(name).empty();
}

[[nodiscard]] bool hasSecurityHeader(HttpResponse& response, std::string_view name) noexcept {
    return !response.header(name).empty();
}

template <typename Target>
void applySecurityHeadersTo(Target& target, const SecurityHeadersOptions& options) {
    const auto setHeader = [&target, &options](
                               std::string_view name,
                               std::string_view value,
                               bool skipEmpty) {
        if (skipEmpty && value.empty()) {
            return;
        }
        if (!options.overwriteExisting && hasSecurityHeader(target, name)) {
            return;
        }
        target.header(name, value);
    };

    if (options.contentTypeOptions) {
        setHeader("X-Content-Type-Options", "nosniff", true);
    }
    if (options.frameOptions) {
        setHeader("X-Frame-Options", "DENY", true);
    }
    if (options.strictTransportSecurity) {
        setHeader("Strict-Transport-Security", "max-age=31536000; includeSubDomains", true);
    }
    if (options.xssProtection) {
        setHeader("X-XSS-Protection", "0", true);
    }

    setHeader("Content-Security-Policy", options.contentSecurityPolicy, true);
    setHeader("Referrer-Policy", options.referrerPolicy, true);
    setHeader("Permissions-Policy", options.permissionsPolicy, true);

    for (const auto& header : options.customHeaders) {
        setHeader(header.name, header.value, false);
    }
}

}  // namespace

void applySecurityHeaders(Context& context, const SecurityHeadersOptions& options) {
    applySecurityHeadersTo(context, options);
}

void applySecurityHeaders(HttpResponse& response, const SecurityHeadersOptions& options) {
    applySecurityHeadersTo(response, options);
}

Task<void> SecurityHeadersMiddleware::handle(Context& context, Next& next) {
    applySecurityHeaders(context);
    co_await next();
    applySecurityHeaders(context.res());
}

}  // namespace ruvia
