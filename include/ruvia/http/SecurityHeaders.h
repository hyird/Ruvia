#pragma once

#include <span>
#include <string_view>

#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/MiddlewareRuntime.h"
#include "ruvia/http/Next.h"

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
    Task<HttpResponse> handle(Context& context, const Next& next);
};

}  // namespace ruvia
