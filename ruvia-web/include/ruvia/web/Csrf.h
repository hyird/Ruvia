#pragma once

#include <memory_resource>
#include <string>

#include "ruvia/core/Task.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/web/Next.h"

namespace ruvia {

struct CsrfProtectionConfig final {
    std::string cookieName{"XSRF-TOKEN"};
    std::string headerName{"X-XSRF-TOKEN"};
};

// Stateless CSRF protection using the double-submit-cookie pattern (no
// server-side session store needed, so it works across SO_REUSEPORT workers).
// A safe request (GET/HEAD/OPTIONS) without a token cookie is issued a fresh
// one (readable by JavaScript so a SPA can echo it). An unsafe request must
// repeat that cookie's value in the header; a missing or mismatched token is
// rejected with 403. Register on a controller, group, or route that should
// enforce browser XSRF checks. Cookie and header names default to "XSRF-TOKEN"
// and "X-XSRF-TOKEN" and can be rebranded per app.
class CsrfProtection final : public Middleware<CsrfProtection> {
public:
    CsrfProtection();
    explicit CsrfProtection(const CsrfProtectionConfig& config);

    CsrfProtection(const CsrfProtection&) = delete;
    CsrfProtection& operator=(const CsrfProtection&) = delete;
    CsrfProtection(CsrfProtection&&) = delete;
    CsrfProtection& operator=(CsrfProtection&&) = delete;

    Task<void> handle(Context& c, Next& next);

private:
    struct ConfigStorage final {
        ConfigStorage(const CsrfProtectionConfig& source, std::pmr::memory_resource* resource);

        std::pmr::string cookieName;
        std::pmr::string headerName;

    private:
        struct ValidatedConfig final {
            const CsrfProtectionConfig* source;
        };

        [[nodiscard]] static ValidatedConfig validate(const CsrfProtectionConfig& source);
        ConfigStorage(ValidatedConfig validated, std::pmr::memory_resource* resource);
    };

    ConfigStorage config_;
};

}  // namespace ruvia
