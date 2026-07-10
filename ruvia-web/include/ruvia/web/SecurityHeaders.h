#pragma once

#include <span>
#include <string_view>

#include "ruvia/core/Task.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/MiddlewareRuntime.h"
#include "ruvia/web/Next.h"

namespace ruvia {

struct SecurityHeader final {
    std::string_view name;
    std::string_view value;
};

struct SecurityHeadersOptions final {
    bool contentTypeOptions = true;
    bool frameOptions = true;
    bool strictTransportSecurity = true;
    bool xssProtection = true;

    std::string_view contentSecurityPolicy = "default-src 'self'";
    std::string_view referrerPolicy = "strict-origin-when-cross-origin";
    std::string_view permissionsPolicy = "geolocation=(), microphone=(), camera=()";

    std::span<const SecurityHeader> customHeaders{};
    bool overwriteExisting = false;
};

void applySecurityHeaders(Context& context, const SecurityHeadersOptions& options = {});
void applySecurityHeaders(HttpResponse& response, const SecurityHeadersOptions& options = {});

class SecurityHeadersMiddleware final : public Middleware<SecurityHeadersMiddleware> {
public:
    Task<void> handle(Context& context, Next& next);
};

}  // namespace ruvia
