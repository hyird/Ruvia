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

// Per-worker, per-address fixed-window rule. Configuration remains a plain
// value; each owner validates it once before publishing runtime state.
struct RateLimitRule final {
    std::size_t maxRequests{0};
    std::chrono::milliseconds window{0};
    RateLimitOverflowPolicy overflowPolicy{RateLimitOverflowPolicy::kDeny};
};

namespace detail {

inline constexpr void validateRateLimitRule(const RateLimitRule& rule) {
    if (rule.maxRequests == 0) {
        throw std::invalid_argument("rate limit max requests must be greater than zero");
    }
    if (rule.window.count() <= 0) {
        throw std::invalid_argument("rate limit window must be greater than zero");
    }
    if (rule.overflowPolicy != RateLimitOverflowPolicy::kDeny && rule.overflowPolicy != RateLimitOverflowPolicy::kAllow) {
        throw std::invalid_argument("rate limit overflow policy is invalid");
    }
}

}  // namespace detail

}  // namespace ruvia
