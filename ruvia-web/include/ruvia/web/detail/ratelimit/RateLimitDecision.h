#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/ratelimit/RateLimitKey.h"
#include "ruvia/web/detail/ratelimit/RateLimiter.h"

namespace ruvia::detail {

[[nodiscard]] inline RateLimitDecision decideRequestRateLimit(
    RateLimiter* limiter, std::string_view remoteAddress) noexcept {
    if (limiter == nullptr || !limiter->hasDefaultRule()) {
        return RateLimitDecision::allow();
    }
    char keyBuffer[kRateLimitKeyBufferBytes];
    return limiter->allowDefault(rateLimitKeyFor(remoteAddress, keyBuffer));
}

[[nodiscard]] HttpErrorInfo rateLimitRejectionError() noexcept;

void applyRateLimitRejectionHeaders(HttpResponse& response, const RateLimitRejection& rejection);

void applyRouteRateLimitRejectionHeaders(
    HttpResponse& response, const RateLimitRejection& rejection, std::size_t maxRequests);

}  // namespace ruvia::detail
