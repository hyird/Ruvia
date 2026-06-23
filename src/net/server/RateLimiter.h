#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ruvia::detail {

[[nodiscard]] inline std::int64_t rateLimiterNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Per-worker, per-client token-bucket rate limiter. Each worker owns one
// io_context and runs single-threaded, so the bucket map needs no locks. The
// bucket holds up to `maxRequests` tokens and refills at maxRequests/window;
// every request spends one token and is rejected (429) when the bucket is empty.
// Because the kernel spreads connections across workers (SO_REUSEPORT), the
// effective limit is per worker — i.e. roughly maxRequests x worker count. Stale
// buckets are evicted on the periodic worker scan so memory tracks active
// clients, not all clients ever seen.
class RateLimiter final {
public:
    RateLimiter(std::size_t maxRequests, std::int64_t windowMs, std::pmr::memory_resource* resource)
        : maxRequests_(maxRequests),
          windowMs_(windowMs > 0 ? windowMs : 1),
          buckets_(resource) {}

    [[nodiscard]] bool enabled() const noexcept {
        return maxRequests_ > 0;
    }

    // Consumes one token for `key`; false means the limit is exceeded.
    [[nodiscard]] bool allow(std::string_view key) {
        if (maxRequests_ == 0) {
            return true;
        }
        const auto now = rateLimiterNowMs();
        auto it = buckets_.find(key);
        if (it == buckets_.end()) {
            it = buckets_
                     .emplace(
                         std::pmr::string(key, buckets_.get_allocator().resource()),
                         Bucket{static_cast<double>(maxRequests_), now, now})
                     .first;
        }
        Bucket& bucket = it->second;
        refill(bucket, now);
        bucket.lastSeenMs = now;
        if (bucket.tokens < 1.0) {
            return false;
        }
        bucket.tokens -= 1.0;
        return true;
    }

    // Drops buckets idle for longer than a window; they are at full tokens and
    // carry no state, so eviction is lossless. Invoked from the worker scan.
    void evictStale() {
        if (maxRequests_ == 0 || buckets_.empty()) {
            return;
        }
        const auto now = rateLimiterNowMs();
        for (auto it = buckets_.begin(); it != buckets_.end();) {
            it = now - it->second.lastSeenMs > windowMs_ ? buckets_.erase(it) : std::next(it);
        }
    }

    static void evictStaleThunk(void* target) noexcept {
        static_cast<RateLimiter*>(target)->evictStale();
    }

private:
    struct StringHash final {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    struct Bucket final {
        double tokens;
        std::int64_t lastRefillMs;
        std::int64_t lastSeenMs;
    };

    void refill(Bucket& bucket, std::int64_t now) const noexcept {
        const auto elapsed = now - bucket.lastRefillMs;
        if (elapsed <= 0) {
            return;
        }
        const double perMs = static_cast<double>(maxRequests_) / static_cast<double>(windowMs_);
        bucket.tokens = std::min(static_cast<double>(maxRequests_), bucket.tokens + static_cast<double>(elapsed) * perMs);
        bucket.lastRefillMs = now;
    }

    std::size_t maxRequests_;
    std::int64_t windowMs_;
    std::pmr::unordered_map<std::pmr::string, Bucket, StringHash, std::equal_to<>> buckets_;
};

}  // namespace ruvia::detail
