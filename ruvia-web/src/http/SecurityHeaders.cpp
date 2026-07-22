#include "ruvia/web/SecurityHeaders.h"

#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/detail/http/ContextAccess.h"

namespace ruvia {
namespace {

[[nodiscard]] bool hasSecurityHeader(Context& context, std::string_view name) {
    return detail::ContextAccess::hasResponseHeader(context, name);
}

[[nodiscard]] bool hasSecurityHeader(HttpResponse& response, std::string_view name) noexcept {
    return response.header(name).has_value();
}

template <typename Target>
void applySecurityHeadersTo(
    Target& target,
    const SecurityHeadersOptions& options,
    bool secureTransport) {
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
    // RFC 6797 section 7.2: an HSTS host MUST NOT send STS over a
    // non-secure transport. This decision requires Context connection metadata.
    if (options.strictTransportSecurity && secureTransport) {
        setHeader("Strict-Transport-Security", "max-age=31536000; includeSubDomains", true);
    }
    switch (options.legacyXssFilter) {
        case LegacyXssFilterPolicy::kDisable:
            setHeader("X-XSS-Protection", "0", true);
            break;
        case LegacyXssFilterPolicy::kOmitHeader:
            break;
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
    const auto connection = getConnInfo(context);
    applySecurityHeadersTo(
        context,
        options,
        connection.tls() != nullptr);
}

Task<void> SecurityHeadersMiddleware::handle(Context& context, Next& next) {
    const auto connection = getConnInfo(context);
    const bool secureTransport = connection.tls() != nullptr;
    applySecurityHeadersTo(context, SecurityHeadersOptions{}, secureTransport);
    co_await next();
    applySecurityHeadersTo(
        detail::ContextAccess::responseStorage(context),
        SecurityHeadersOptions{},
        secureTransport);
}

}  // namespace ruvia
