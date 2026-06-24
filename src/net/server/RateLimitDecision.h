#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/app/App.h"
#include "ruvia/app/Task.h"
#include "RateLimiter.h"
#include "../../redis/RedisInternal.h"

#ifdef RUVIA_ENABLE_REDIS
#include "RedisRateLimit.h"
#endif

namespace ruvia::detail {

// Decide whether a request from `remoteAddress` is permitted under the configured
// rate limit: Redis-backed (global, shared across workers/instances) when a
// redisAlias is set and the Redis feature is compiled in, otherwise the per-worker
// local token bucket. Callers check limiter.enabled() first and keep their own
// (protocol-specific) rejection handling; only this allow/deny decision is shared
// between the HTTP/1 and HTTP/2 dispatch paths so it lives in exactly one place.
[[nodiscard]] inline Task<bool> rateLimitRequestAllowed(
    RateLimiter& limiter,
    [[maybe_unused]] RedisRegistry* redis,
    const HttpServerOptions::RateLimit& rateLimit,
    std::string_view remoteAddress,
    [[maybe_unused]] std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_REDIS
    if (redis != nullptr && !rateLimit.redisAlias.empty()) {
        std::pmr::string rateKey(resource);
        rateKey.append("rl:");
        rateKey.append(remoteAddress.data(), remoteAddress.size());
        co_return co_await redisRateLimitAllow(
            redis->get(rateLimit.redisAlias, resource),
            rateKey,
            rateLimit.maxRequests,
            rateLimit.window.count(),
            rateLimit.redisFailOpen);
    }
#endif
    co_return limiter.allow(remoteAddress);
}

}  // namespace ruvia::detail
