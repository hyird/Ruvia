#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/task.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/middleware.h"

namespace ruvia {

class context;
struct security_header final {
    std::string name_{};
    std::string value_{};
};

// Legacy browser XSS filters can introduce vulnerabilities in otherwise safe
// pages. Modern policy either emits X-XSS-Protection: 0 to disable the obsolete
// auditor, or omits the obsolete header entirely for clients that ignore it.
enum class xss_protection_header_policy : std::uint8_t {
    emit_disabled,
    omit,
};

enum class default_security_header_policy : std::uint8_t {
    omit,
    emit_default,
};

enum class security_header_conflict_policy : std::uint8_t {
    preserve_existing,
    replace_existing,
};

struct security_headers_config final {
    default_security_header_policy content_type_options_header_ =
        default_security_header_policy::emit_default;
    default_security_header_policy frame_options_header_ = default_security_header_policy::emit_default;
    // Emitted only for requests received over TLS. Plain HTTP responses must
    // never carry Strict-Transport-Security.
    default_security_header_policy strict_transport_security_header_ =
        default_security_header_policy::emit_default;
    xss_protection_header_policy xss_protection_header_ = xss_protection_header_policy::emit_disabled;

    std::string content_security_policy_{"default-src 'self'"};
    std::string referrer_policy_{"strict-origin-when-cross-origin"};
    std::string permissions_policy_{"geolocation=(), microphone=(), camera=()"};

    std::vector<security_header> custom_headers_{};
    security_header_conflict_policy existing_headers_ = security_header_conflict_policy::preserve_existing;
};

// Registered app-wide with the defaults as `app().use<security_headers_middleware>()`,
// or with an owning policy as `app().use<security_headers_middleware>(config)`.
class security_headers_middleware final {
public:
    // A 404 is a response to an attacker-reachable URL like any other, so it
    // needs the same CSP, frame and referrer policy a matched route gets. CORS
    // follows the same response-layer rule for unmatched requests.
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    security_headers_middleware();
    explicit security_headers_middleware(const security_headers_config& config);

    security_headers_middleware(const security_headers_middleware&) = delete;
    security_headers_middleware& operator=(const security_headers_middleware&) = delete;
    security_headers_middleware(security_headers_middleware&&) = delete;
    security_headers_middleware& operator=(security_headers_middleware&&) = delete;

    task<void> handle(context& context, next& next);

private:
    security_headers_middleware(
        const security_headers_config& source_value, std::pmr::memory_resource* resource);

    bool emit_content_type_options_{true};
    bool emit_frame_options_{true};
    bool emit_strict_transport_security_{true};
    bool emit_disabled_xss_protection_{true};
    bool replace_existing_{false};
    std::pmr::string content_security_policy_;
    std::pmr::string referrer_policy_;
    std::pmr::string permissions_policy_;
    std::pmr::vector<http_header> custom_headers_;
};

}  // namespace ruvia
