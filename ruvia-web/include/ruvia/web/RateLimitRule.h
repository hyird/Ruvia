#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace ruvia {

// Each worker preallocates this many fixed-window key slots when either the
// app-wide default rule or a route-specific rate-limit middleware is present.
inline constexpr std::size_t kDefaultRateLimitSlotsPerWorker = 8192;

enum class RateLimitOverflowPolicy : std::uint8_t {
    kDeny,
    kAllow,
};

// Per-worker, per-address fixed-window rule. Every worker owns an independent
// allocation-free key table, so the admitted process-wide total depends on how
// connections are distributed across workers. Absence, not a zero request
// count, disables the app-level rule. The overflow policy applies when a worker
// cannot represent or allocate a key in its fixed table.
class RateLimitRule final {
public:
    [[nodiscard]] static constexpr RateLimitRule fixedWindow(
        std::size_t maxRequests,
        std::chrono::milliseconds window,
        RateLimitOverflowPolicy overflowPolicy =
            RateLimitOverflowPolicy::kDeny) {
        if (maxRequests == 0) {
            throw std::invalid_argument(
                "rate limit max requests must be greater than zero");
        }
        if (window.count() <= 0) {
            throw std::invalid_argument(
                "rate limit window must be greater than zero");
        }
        return RateLimitRule(maxRequests, window, overflowPolicy);
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
    constexpr RateLimitRule(
        std::size_t maxRequests,
        std::chrono::milliseconds window,
        RateLimitOverflowPolicy overflowPolicy) noexcept
        : maxRequests_(maxRequests),
          window_(window),
          overflowPolicy_(overflowPolicy) {}

    std::size_t maxRequests_;
    std::chrono::milliseconds window_;
    RateLimitOverflowPolicy overflowPolicy_;
};

}  // namespace ruvia
