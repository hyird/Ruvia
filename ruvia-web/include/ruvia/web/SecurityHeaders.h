#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/Task.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/web/Next.h"

namespace ruvia {

struct SecurityHeader final {
    std::string name{};
    std::string value{};
};

// Legacy browser XSS filters can introduce vulnerabilities in otherwise safe
// pages. Modern policy either emits X-XSS-Protection: 0 to disable the obsolete
// auditor, or omits the obsolete header entirely for clients that ignore it.
enum class XssProtectionHeaderPolicy : std::uint8_t {
    kEmitDisabled,
    kOmit,
};

enum class DefaultSecurityHeaderPolicy : std::uint8_t {
    kOmit,
    kEmitDefault,
};

enum class SecurityHeaderConflictPolicy : std::uint8_t {
    kPreserveExisting,
    kReplaceExisting,
};

struct SecurityHeadersConfig final {
    DefaultSecurityHeaderPolicy contentTypeOptionsHeader =
        DefaultSecurityHeaderPolicy::kEmitDefault;
    DefaultSecurityHeaderPolicy frameOptionsHeader = DefaultSecurityHeaderPolicy::kEmitDefault;
    // Emitted only for requests received over TLS. Plain HTTP responses must
    // never carry Strict-Transport-Security.
    DefaultSecurityHeaderPolicy strictTransportSecurityHeader =
        DefaultSecurityHeaderPolicy::kEmitDefault;
    XssProtectionHeaderPolicy xssProtectionHeader = XssProtectionHeaderPolicy::kEmitDisabled;

    std::string contentSecurityPolicy{"default-src 'self'"};
    std::string referrerPolicy{"strict-origin-when-cross-origin"};
    std::string permissionsPolicy{"geolocation=(), microphone=(), camera=()"};

    std::vector<SecurityHeader> customHeaders{};
    SecurityHeaderConflictPolicy existingHeaders = SecurityHeaderConflictPolicy::kPreserveExisting;
};

void applySecurityHeaders(Context& context, const SecurityHeadersConfig& config = {});

// Registered app-wide with the defaults as `app().use<SecurityHeadersMiddleware>()`,
// or with an owning policy as `app().use<SecurityHeadersMiddleware>(config)`.
class SecurityHeadersMiddleware final : public Middleware<SecurityHeadersMiddleware> {
public:
    // A 404 is a response to an attacker-reachable URL like any other, so it
    // needs the same CSP, frame and referrer policy a matched route gets. CORS
    // follows the same response-layer rule for unmatched requests.
    static constexpr bool ruviaRunsOnUnmatchedRequests = true;

    SecurityHeadersMiddleware();
    explicit SecurityHeadersMiddleware(const SecurityHeadersConfig& config);

    SecurityHeadersMiddleware(const SecurityHeadersMiddleware&) = delete;
    SecurityHeadersMiddleware& operator=(const SecurityHeadersMiddleware&) = delete;
    SecurityHeadersMiddleware(SecurityHeadersMiddleware&&) = delete;
    SecurityHeadersMiddleware& operator=(SecurityHeadersMiddleware&&) = delete;

    Task<void> handle(Context& context, Next& next);

private:
    struct StoredHeader final {
        StoredHeader(
            std::string_view name, std::string_view value, std::pmr::memory_resource* resource);

        std::pmr::string name;
        std::pmr::string value;
    };

    struct ConfigStorage final {
        ConfigStorage(const SecurityHeadersConfig& source, std::pmr::memory_resource* resource);

        bool emitContentTypeOptions;
        bool emitFrameOptions;
        bool emitStrictTransportSecurity;
        bool emitDisabledXssProtection;
        bool replaceExisting;
        std::pmr::string contentSecurityPolicy;
        std::pmr::string referrerPolicy;
        std::pmr::string permissionsPolicy;
        std::pmr::vector<StoredHeader> customHeaders;

    private:
        struct ValidatedConfig final {
            const SecurityHeadersConfig* source;
            bool emitContentTypeOptions;
            bool emitFrameOptions;
            bool emitStrictTransportSecurity;
            bool emitDisabledXssProtection;
            bool replaceExisting;
        };

        [[nodiscard]] static ValidatedConfig validate(const SecurityHeadersConfig& source);
        ConfigStorage(ValidatedConfig source, std::pmr::memory_resource* resource);
    };

    ConfigStorage config_;
};

}  // namespace ruvia
