#pragma once

#include <memory_resource>
#include <string>

#include "ruvia/core/task.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/next.h"

namespace ruvia {

struct csrf_protection_config final {
    std::string cookie_name_{"XSRF-TOKEN"};
    std::string header_name_{"X-XSRF-TOKEN"};
};

// Stateless CSRF protection using the double-submit-cookie pattern (no
// server-side session store needed, so it works across SO_REUSEPORT workers).
// A safe request (GET/HEAD/OPTIONS) without a token cookie is issued a fresh
// one (readable by JavaScript so a SPA can echo it). An unsafe request must
// repeat that cookie's value in the header; a missing or mismatched token is
// rejected with 403. Register on a controller, group, or route that should
// enforce browser XSRF checks. Cookie and header names default to "XSRF-TOKEN"
// and "X-XSRF-TOKEN" and can be rebranded per app.
class csrf_protection final : public middleware {
public:
    csrf_protection();
    explicit csrf_protection(const csrf_protection_config& config);

    csrf_protection(const csrf_protection&) = delete;
    csrf_protection& operator=(const csrf_protection&) = delete;
    csrf_protection(csrf_protection&&) = delete;
    csrf_protection& operator=(csrf_protection&&) = delete;

    task<void> handle(context& c, next& next);

private:
    struct config_storage_type final {
        config_storage_type(const csrf_protection_config& source_value, std::pmr::memory_resource* resource);

        std::pmr::string cookie_name_;
        std::pmr::string header_name_;

    private:
        struct validated_config_type final {
            const csrf_protection_config* source_;
        };

        [[nodiscard]] static validated_config_type validate(const csrf_protection_config& source);
        config_storage_type(validated_config_type validated, std::pmr::memory_resource* resource);
    };

    config_storage_type config_;
};

}  // namespace ruvia
