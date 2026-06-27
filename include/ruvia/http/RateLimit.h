#pragma once

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/MiddlewareRuntime.h"
#include "ruvia/http/Next.h"
#include "ruvia/memory/ProcessResource.h"

namespace ruvia {

namespace detail {

[[nodiscard]] inline std::int64_t routeRateLimitNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void setUnsignedHeader(HttpResponse& response, std::string_view name, std::uint64_t value) {
    char buffer[24];
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec == std::errc{}) {
        response.setHeader(name, std::string_view(buffer, static_cast<std::size_t>(ptr - buffer)));
    }
}

}  // namespace detail

template <typename Derived>
class RouteRateLimitMiddleware : public Middleware<Derived> {
public:
    Task<HttpResponse> handle(Context& context, const Next& next) {
        static_assert(
            Derived::ruviaRateLimitMaxRequests > 0,
            "route rate limit max requests must be greater than 0");
        static_assert(
            Derived::ruviaRateLimitWindowMs > 0,
            "route rate limit window must be greater than 0ms");

        const auto check = allow(context.remoteAddress());
        if (!check.allowed) {
            auto response = context.error(429, "too_many_requests", "rate limit exceeded");
            detail::setUnsignedHeader(response, "Retry-After", retryAfterSeconds(check.resetAfterMs));
            detail::setUnsignedHeader(response, "X-RateLimit-Limit", Derived::ruviaRateLimitMaxRequests);
            detail::setUnsignedHeader(response, "X-RateLimit-Remaining", 0);
            detail::setUnsignedHeader(response, "X-RateLimit-Reset", retryAfterSeconds(check.resetAfterMs));
            co_return response;
        }

        co_return co_await next(context);
    }

private:
    struct CheckResult final {
        bool allowed{false};
        std::int64_t resetAfterMs{1};
    };

    [[nodiscard]] static std::uint64_t currentSlot(std::int64_t nowMs) noexcept {
        const auto safeNow = nowMs < 0 ? std::int64_t{0} : nowMs;
        return static_cast<std::uint64_t>(safeNow / Derived::ruviaRateLimitWindowMs);
    }

    [[nodiscard]] static std::int64_t resetAfterMs(std::int64_t nowMs, std::uint64_t slot) noexcept {
        const auto nextWindowMs = static_cast<std::int64_t>(slot + 1) * Derived::ruviaRateLimitWindowMs;
        const auto remaining = nextWindowMs - nowMs;
        return remaining <= 0 ? 1 : remaining;
    }

    [[nodiscard]] static std::uint64_t retryAfterSeconds(std::int64_t resetAfterMs) noexcept {
        return static_cast<std::uint64_t>((resetAfterMs <= 0 ? 1 : resetAfterMs + 999) / 1000);
    }

    class Shard final {
    public:
        Shard(std::pmr::memory_resource* resource = detail::processResource())
            : buckets_(resource) {}

        [[nodiscard]] CheckResult allow(std::string_view key, std::int64_t nowMs) {
            const auto slot = currentSlot(nowMs);
            ++requestsSinceGc_;
            if (requestsSinceGc_ >= kGcIntervalRequests) {
                requestsSinceGc_ = 0;
                evictOld(slot);
            }

            auto it = buckets_.find(key);
            if (it == buckets_.end()) {
                it = buckets_
                         .emplace(
                             std::pmr::string(key, buckets_.get_allocator().resource()),
                             Bucket{slot, 0})
                         .first;
            }

            auto& bucket = it->second;
            if (bucket.slot != slot) {
                bucket.slot = slot;
                bucket.count = 0;
            }

            if (bucket.count >= Derived::ruviaRateLimitMaxRequests) {
                return CheckResult{.allowed = false, .resetAfterMs = resetAfterMs(nowMs, slot)};
            }

            ++bucket.count;
            return CheckResult{.allowed = true, .resetAfterMs = resetAfterMs(nowMs, slot)};
        }

    private:
        struct StringHash final {
            using is_transparent = void;

            [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }
        };

        struct Bucket final {
            std::uint64_t slot{0};
            std::uint64_t count{0};
        };

        std::pmr::unordered_map<std::pmr::string, Bucket, StringHash, std::equal_to<>> buckets_;
        std::uint32_t requestsSinceGc_{0};

        static constexpr std::uint32_t kGcIntervalRequests = 1024;

        void evictOld(std::uint64_t current) {
            for (auto it = buckets_.begin(); it != buckets_.end();) {
                it = it->second.slot + 2 < current ? buckets_.erase(it) : std::next(it);
            }
        }
    };

    [[nodiscard]] CheckResult allow(std::string_view key) {
        const auto nowMs = detail::routeRateLimitNowMs();
        return shardFor(key).allow(key, nowMs);
    }

    [[nodiscard]] static constexpr std::size_t shardIndex(std::string_view key) noexcept {
        return std::hash<std::string_view>{}(key) & (kShardCount - 1);
    }

    [[nodiscard]] static Shard& shardFor(std::string_view key) noexcept {
        thread_local Shard shards[kShardCount]{};
        return shards[shardIndex(key)];
    }

    static constexpr std::size_t kShardCount = 64;
};

template <std::size_t MaxRequests, std::int64_t WindowMs>
class RouteRateLimit final : public RouteRateLimitMiddleware<RouteRateLimit<MaxRequests, WindowMs>> {
public:
    static constexpr std::size_t ruviaRateLimitMaxRequests = MaxRequests;
    static constexpr std::int64_t ruviaRateLimitWindowMs = WindowMs;
};

}  // namespace ruvia

#define RUVIA_ROUTE_RATE_LIMIT(name, max_requests, window_ms) \
    class name final : public ::ruvia::RouteRateLimitMiddleware<name> { \
    public: \
        static constexpr ::std::size_t ruviaRateLimitMaxRequests = max_requests; \
        static constexpr ::std::int64_t ruviaRateLimitWindowMs = window_ms; \
    }
