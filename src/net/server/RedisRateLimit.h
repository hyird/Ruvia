#pragma once

#ifdef RUVIA_ENABLE_REDIS

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/app/Task.h"
#include "ruvia/redis/RedisHandle.h"
#include "ruvia/redis/RedisTypes.h"

namespace ruvia::detail {

// Global (cross-worker / cross-instance) fixed-window rate limit in Redis. One
// atomic round-trip: INCR the per-key counter and, on first hit in the window,
// arm a PEXPIRE; the request is allowed while the counter stays within the
// limit. Fails open — a Redis error allows the request rather than blocking all
// traffic when the limiter is unavailable.
[[nodiscard]] inline Task<bool> redisRateLimitAllow(
    RedisHandle handle,
    std::string_view key,
    std::size_t maxRequests,
    std::int64_t windowMs) {
    static constexpr std::string_view kScript =
        "local c = redis.call('INCR', KEYS[1]) "
        "if c == 1 then redis.call('PEXPIRE', KEYS[1], ARGV[1]) end "
        "return c";
    char windowBuffer[24];
    const auto [ptr, ec] = std::to_chars(
        windowBuffer, windowBuffer + sizeof(windowBuffer), windowMs <= 0 ? 1 : windowMs);
    if (ec != std::errc{}) {
        co_return true;
    }
    const std::array<std::string_view, 1> keys{key};
    const std::array<std::string_view, 1> args{std::string_view(windowBuffer, static_cast<std::size_t>(ptr - windowBuffer))};
    try {
        const auto value = co_await handle.eval(kScript, keys, args);
        const auto count = value.integer();
        co_return count <= 0 || static_cast<std::size_t>(count) <= maxRequests;
    } catch (...) {
        co_return true;
    }
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_REDIS
