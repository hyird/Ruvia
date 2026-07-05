#pragma once

#include <string_view>

#include "RateLimitKey.h"
#include "RateLimiter.h"

namespace ruvia::detail {

[[nodiscard]] inline RateLimitCheck rateLimitRequestAllowed(
    RateLimiter* limiter,
    std::string_view remoteAddress) noexcept {
    if (limiter == nullptr || !limiter->enabled()) {
        return RateLimitCheck{};
    }
    char keyBuffer[kRateLimitKeyBufferBytes];
    return limiter->allowGlobal(rateLimitKeyFor(remoteAddress, keyBuffer));
}

}  // namespace ruvia::detail
