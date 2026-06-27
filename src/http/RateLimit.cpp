#include "ruvia/http/RateLimit.h"

#include "ContextInternal.h"
#include "../net/server/RateLimiter.h"

namespace ruvia::detail {

RouteRateLimitResult checkRouteRateLimit(
    Context& context,
    RouteRateLimitOptions options) noexcept {
    auto* limiter = ContextAccess::rateLimiter(context);
    if (limiter == nullptr || options.maxRequests == 0) {
        return RouteRateLimitResult{};
    }

    const auto check = limiter->allowRoute(
        ContextAccess::routeRateLimitScope(context),
        context.remoteAddress(),
        options.maxRequests,
        options.windowMs);
    return RouteRateLimitResult{
        .allowed = check.allowed,
        .resetAfterMs = check.resetAfterMs};
}

}  // namespace ruvia::detail
