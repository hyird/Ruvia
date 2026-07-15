#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/server/RateLimitKey.h"
#include "ruvia/web/detail/server/RateLimiter.h"

namespace ruvia::detail {

[[nodiscard]] inline RateLimitDecision decideRequestRateLimit(
    RateLimiter* limiter,
    std::string_view remoteAddress) noexcept {
    if (limiter == nullptr || !limiter->enabled()) {
        return RateLimitDecision::allow();
    }
    char keyBuffer[kRateLimitKeyBufferBytes];
    return limiter->allowGlobal(rateLimitKeyFor(remoteAddress, keyBuffer));
}

[[nodiscard]] HttpErrorInfo rateLimitRejectionError() noexcept;

void applyRateLimitRejectionHeaders(
    HttpResponse& response,
    const RateLimitRejection& rejection);

void applyRouteRateLimitRejectionHeaders(
    HttpResponse& response,
    const RateLimitRejection& rejection,
    std::size_t maxRequests);

}  // namespace ruvia::detail
