#pragma once

#include <cstddef>
#include <cstdint>
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
    std::string name;
    std::string value;
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
    DefaultSecurityHeaderPolicy contentTypeOptionsHeader = DefaultSecurityHeaderPolicy::kEmitDefault;
    DefaultSecurityHeaderPolicy frameOptionsHeader = DefaultSecurityHeaderPolicy::kEmitDefault;
    // Emitted only for requests received over TLS. Plain HTTP responses must
    // never carry Strict-Transport-Security.
    DefaultSecurityHeaderPolicy strictTransportSecurityHeader = DefaultSecurityHeaderPolicy::kEmitDefault;
    XssProtectionHeaderPolicy xssProtectionHeader = XssProtectionHeaderPolicy::kEmitDisabled;

    std::string contentSecurityPolicy{"default-src 'self'"};
    std::string referrerPolicy{"strict-origin-when-cross-origin"};
    std::string permissionsPolicy{"geolocation=(), microphone=(), camera=()"};

    std::vector<SecurityHeader> customHeaders;
    SecurityHeaderConflictPolicy existingHeaders = SecurityHeaderConflictPolicy::kPreserveExisting;
};

void applySecurityHeaders(Context& context, const SecurityHeadersConfig& config = {});

// Registered app-wide with the defaults as `app().use<SecurityHeadersMiddleware>()`,
// or with an owning policy as `app().use<SecurityHeadersMiddleware>(config)`.
class SecurityHeadersMiddleware final : public Middleware<SecurityHeadersMiddleware> {
public:
    // A 404 is a response to an attacker-reachable URL like any other, so it
    // needs the same CSP, frame and referrer policy a matched route gets.
    // Without this the headers were silently absent from every 404, 405 and 501
    // -- which is also why CORS had to be applied in the response layer instead
    // of as middleware.
    static constexpr bool ruviaRunsOnUnmatchedRequests = true;

    SecurityHeadersMiddleware();
    explicit SecurityHeadersMiddleware(SecurityHeadersConfig config);

    Task<void> handle(Context& context, Next& next);

private:
    SecurityHeadersConfig config_{};
};

}  // namespace ruvia
