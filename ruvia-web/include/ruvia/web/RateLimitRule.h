#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace ruvia {

// Each worker preallocates this many fixed-window key entries when either the
// app-wide default rule or a route-specific rate-limit middleware is present.
inline constexpr std::size_t kDefaultRateLimitCapacityPerWorker = 8192;

enum class RateLimitOverflowPolicy : std::uint8_t {
    kDeny,
    kAllow,
};

struct RateLimitFixedWindowOptions final {
    std::size_t maxRequests{0};
    std::chrono::milliseconds window{0};
    RateLimitOverflowPolicy overflowPolicy{RateLimitOverflowPolicy::kDeny};
};

// Per-worker, per-address fixed-window rule. Every worker owns an independent
// allocation-free key table, so the admitted process-wide total depends on how
// connections are distributed across workers. Absence, not a zero request
// count, disables the app-level rule. The overflow policy applies when a worker
// cannot represent or allocate a key in its fixed table.
class RateLimitRule final {
public:
    [[nodiscard]] static constexpr RateLimitRule fixedWindow(RateLimitFixedWindowOptions options) {
        if (options.maxRequests == 0) {
            throw std::invalid_argument("rate limit max requests must be greater than zero");
        }
        if (options.window.count() <= 0) {
            throw std::invalid_argument("rate limit window must be greater than zero");
        }
        if (options.overflowPolicy != RateLimitOverflowPolicy::kDeny && options.overflowPolicy != RateLimitOverflowPolicy::kAllow) {
            throw std::invalid_argument("rate limit overflow policy is invalid");
        }
        return RateLimitRule(options.maxRequests, options.window, options.overflowPolicy);
    }

    [[nodiscard]] constexpr std::size_t maxRequests() const noexcept {
        return maxRequests_;
    }

    [[nodiscard]] constexpr std::chrono::milliseconds window() const noexcept {
        return window_;
    }

    [[nodiscard]] constexpr RateLimitOverflowPolicy overflowPolicy() const noexcept {
        return overflowPolicy_;
    }

private:
    constexpr RateLimitRule(std::size_t maxRequests, std::chrono::milliseconds window, RateLimitOverflowPolicy overflowPolicy) noexcept
        : maxRequests_(maxRequests),
          window_(window),
          overflowPolicy_(overflowPolicy) {}

    std::size_t maxRequests_;
    std::chrono::milliseconds window_;
    RateLimitOverflowPolicy overflowPolicy_;
};

}  // namespace ruvia
