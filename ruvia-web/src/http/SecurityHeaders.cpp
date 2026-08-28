#include "ruvia/web/SecurityHeaders.h"

#include <stdexcept>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/util/RegistrationResource.h"

namespace ruvia {
namespace {

[[nodiscard]] bool hasSecurityHeader(Context& context, std::string_view name) {
    return detail::ContextAccess::hasResponseHeader(context, name);
}

[[nodiscard]] bool hasSecurityHeader(HttpResponse& response, std::string_view name) noexcept {
    return response.header(name).has_value();
}

[[nodiscard]] bool emitsDefaultSecurityHeader(DefaultSecurityHeaderPolicy policy) {
    switch (policy) {
        case DefaultSecurityHeaderPolicy::kEmitDefault:
            return true;
        case DefaultSecurityHeaderPolicy::kOmit:
            return false;
        default:
            throw std::invalid_argument("default security header policy is invalid");
    }
}

[[nodiscard]] bool replacesExistingSecurityHeader(SecurityHeaderConflictPolicy policy) {
    switch (policy) {
        case SecurityHeaderConflictPolicy::kPreserveExisting:
            return false;
        case SecurityHeaderConflictPolicy::kReplaceExisting:
            return true;
        default:
            throw std::invalid_argument("security header conflict policy is invalid");
    }
}

struct SecurityHeadersFlags final {
    bool emitContentTypeOptions;
    bool emitFrameOptions;
    bool emitStrictTransportSecurity;
    bool emitDisabledXssProtection;
    bool replaceExisting;
};

struct SecurityHeadersPolicy final {
    SecurityHeadersFlags flags;
    std::string_view contentSecurityPolicy;
    std::string_view referrerPolicy;
    std::string_view permissionsPolicy;
};

[[nodiscard]] SecurityHeadersFlags validateSecurityHeadersConfig(
    const SecurityHeadersConfig& config) {
    const bool emitContentTypeOptions = emitsDefaultSecurityHeader(config.contentTypeOptionsHeader);
    const bool emitFrameOptions = emitsDefaultSecurityHeader(config.frameOptionsHeader);
    const bool emitStrictTransportSecurity =
        emitsDefaultSecurityHeader(config.strictTransportSecurityHeader);
    const bool replaceExisting = replacesExistingSecurityHeader(config.existingHeaders);
    bool emitDisabledXssProtection = false;
    switch (config.xssProtectionHeader) {
        case XssProtectionHeaderPolicy::kEmitDisabled:
            emitDisabledXssProtection = true;
            break;
        case XssProtectionHeaderPolicy::kOmit:
            break;
        default:
            throw std::invalid_argument("X-XSS-Protection header policy is invalid");
    }

    const auto validateValue = [](std::string_view value) {
        if (!value.empty() && !isValidHttpHeaderValue(value)) {
            throw std::invalid_argument("security header value is invalid");
        }
    };
    validateValue(config.contentSecurityPolicy);
    validateValue(config.referrerPolicy);
    validateValue(config.permissionsPolicy);
    for (const auto& header : config.customHeaders) {
        if (!isValidHttpHeaderName(header.name)) {
            throw std::invalid_argument("custom security header name is invalid");
        }
        if (!isValidHttpHeaderValue(header.value)) {
            throw std::invalid_argument("custom security header value is invalid");
        }
    }
    return SecurityHeadersFlags{
        .emitContentTypeOptions = emitContentTypeOptions,
        .emitFrameOptions = emitFrameOptions,
        .emitStrictTransportSecurity = emitStrictTransportSecurity,
        .emitDisabledXssProtection = emitDisabledXssProtection,
        .replaceExisting = replaceExisting,
    };
}

template <typename Target, typename HeaderRange>
void applySecurityHeadersTo(Target& target, const SecurityHeadersPolicy& policy,
    const HeaderRange& customHeaders, bool secureTransport) {
    const auto setHeader = [&target, replaceExisting = policy.flags.replaceExisting](
                               std::string_view name, std::string_view value, bool skipEmpty) {
        if (skipEmpty && value.empty()) {
            return;
        }
        if (!replaceExisting && hasSecurityHeader(target, name)) {
            return;
        }
        target.header(name, value);
    };

    const auto setSecureTransportHeader = [&setHeader, secureTransport](std::string_view name,
                                              std::string_view value, bool skipEmpty) {
        if (!secureTransport &&
            detail::httpAsciiEqualsIgnoreCase(name, "Strict-Transport-Security")) {
            return;
        }
        setHeader(name, value, skipEmpty);
    };

    if (policy.flags.emitContentTypeOptions) {
        setHeader("X-Content-Type-Options", "nosniff", true);
    }
    if (policy.flags.emitFrameOptions) {
        setHeader("X-Frame-Options", "DENY", true);
    }
    // RFC 6797 section 7.2: an HSTS host MUST NOT send STS over a
    // non-secure transport. This decision requires Context connection metadata.
    if (policy.flags.emitStrictTransportSecurity && secureTransport) {
        setHeader("Strict-Transport-Security", "max-age=31536000; includeSubDomains", true);
    }
    if (policy.flags.emitDisabledXssProtection) {
        setHeader("X-XSS-Protection", "0", true);
    }

    setHeader("Content-Security-Policy", policy.contentSecurityPolicy, true);
    setHeader("Referrer-Policy", policy.referrerPolicy, true);
    setHeader("Permissions-Policy", policy.permissionsPolicy, true);

    for (const auto& header : customHeaders) {
        setSecureTransportHeader(header.name, header.value, false);
    }
}

}  // namespace

SecurityHeadersMiddleware::StoredHeader::StoredHeader(
    std::string_view headerName, std::string_view headerValue, std::pmr::memory_resource* resource)
    : name(headerName, resource),
      value(headerValue, resource) {}

SecurityHeadersMiddleware::ConfigStorage::ValidatedConfig
SecurityHeadersMiddleware::ConfigStorage::validate(const SecurityHeadersConfig& source) {
    const auto flags = validateSecurityHeadersConfig(source);
    return ValidatedConfig{
        .source = &source,
        .emitContentTypeOptions = flags.emitContentTypeOptions,
        .emitFrameOptions = flags.emitFrameOptions,
        .emitStrictTransportSecurity = flags.emitStrictTransportSecurity,
        .emitDisabledXssProtection = flags.emitDisabledXssProtection,
        .replaceExisting = flags.replaceExisting,
    };
}

SecurityHeadersMiddleware::ConfigStorage::ConfigStorage(
    const SecurityHeadersConfig& source, std::pmr::memory_resource* resource)
    : ConfigStorage(validate(source), resource) {}

SecurityHeadersMiddleware::ConfigStorage::ConfigStorage(
    ValidatedConfig validated, std::pmr::memory_resource* resource)
    : emitContentTypeOptions(validated.emitContentTypeOptions),
      emitFrameOptions(validated.emitFrameOptions),
      emitStrictTransportSecurity(validated.emitStrictTransportSecurity),
      emitDisabledXssProtection(validated.emitDisabledXssProtection),
      replaceExisting(validated.replaceExisting),
      contentSecurityPolicy(validated.source->contentSecurityPolicy, resource),
      referrerPolicy(validated.source->referrerPolicy, resource),
      permissionsPolicy(validated.source->permissionsPolicy, resource),
      customHeaders(resource) {
    customHeaders.reserve(validated.source->customHeaders.size());
    for (const auto& header : validated.source->customHeaders) {
        customHeaders.emplace_back(header.name, header.value, resource);
    }
}

void applySecurityHeaders(Context& context, const SecurityHeadersConfig& options) {
    const auto flags = validateSecurityHeadersConfig(options);
    const auto connection = getConnInfo(context);
    applySecurityHeadersTo(context,
        SecurityHeadersPolicy{
            .flags = flags,
            .contentSecurityPolicy = options.contentSecurityPolicy,
            .referrerPolicy = options.referrerPolicy,
            .permissionsPolicy = options.permissionsPolicy,
        },
        options.customHeaders, connection.scheme() == HttpScheme::kHttps);
}

SecurityHeadersMiddleware::SecurityHeadersMiddleware()
    : SecurityHeadersMiddleware(SecurityHeadersConfig{}) {}

SecurityHeadersMiddleware::SecurityHeadersMiddleware(const SecurityHeadersConfig& config)
    : config_(config, detail::registrationResource()) {}

Task<void> SecurityHeadersMiddleware::handle(Context& context, Next& next) {
    const auto connection = getConnInfo(context);
    const bool secureTransport = connection.scheme() == HttpScheme::kHttps;
    const SecurityHeadersPolicy policy{
        .flags =
            SecurityHeadersFlags{
                .emitContentTypeOptions = config_.emitContentTypeOptions,
                .emitFrameOptions = config_.emitFrameOptions,
                .emitStrictTransportSecurity = config_.emitStrictTransportSecurity,
                .emitDisabledXssProtection = config_.emitDisabledXssProtection,
                .replaceExisting = config_.replaceExisting,
            },
        .contentSecurityPolicy = config_.contentSecurityPolicy,
        .referrerPolicy = config_.referrerPolicy,
        .permissionsPolicy = config_.permissionsPolicy,
    };
    applySecurityHeadersTo(context, policy, config_.customHeaders, secureTransport);
    co_await next();
    applySecurityHeadersTo(detail::ContextAccess::responseStorage(context), policy,
        config_.customHeaders, secureTransport);
}

}  // namespace ruvia
