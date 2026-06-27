#pragma once

#include <string_view>

#include "RateLimiter.h"

namespace ruvia::detail {

[[nodiscard]] inline RateLimitCheck rateLimitRequestAllowed(
    RateLimiter* limiter,
    std::string_view remoteAddress) noexcept {
    if (limiter == nullptr || !limiter->enabled()) {
        return RateLimitCheck{};
    }
    return limiter->allowGlobal(remoteAddress);
}

}  // namespace ruvia::detail
