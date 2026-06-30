#include "ruvia/http/RateLimit.h"

#include "ContextInternal.h"
#include "../net/server/RateLimiter.h"

namespace ruvia::detail {

RouteRateLimitResult checkRouteRateLimit(
    Context& context,
    const RouteRateLimitOptions& options) noexcept {
    auto* limiter = ContextAccess::rateLimiter(context);
    if (limiter == nullptr || options.rule.maxRequests == 0) {
        return RouteRateLimitResult{};
    }

    const auto check = limiter->allowRoute(
        ContextAccess::routeRateLimitScope(context),
        context.req().remoteAddress(),
        options.rule);
    return RouteRateLimitResult{
        .allowed = check.allowed,
        .resetAfterMs = check.resetAfterMs};
}

}  // namespace ruvia::detail
