#pragma once

#include <chrono>
#include <cstddef>

namespace ruvia {

inline constexpr std::size_t kDefaultRateLimitSlotCount = 65536;

// Single-process, per-IP fixed-window rate limit rule. maxRequests == 0
// disables the app-level rule. slotCount sizes the shared atomic key table used
// by both app-level and route-level limit checks; when the table is full, the
// limiter follows failClosed.
struct RateLimitRule final {
    std::size_t maxRequests{0};
    std::chrono::milliseconds window{std::chrono::seconds(1)};
    std::size_t slotCount{kDefaultRateLimitSlotCount};
    bool failClosed{true};
};

}  // namespace ruvia
