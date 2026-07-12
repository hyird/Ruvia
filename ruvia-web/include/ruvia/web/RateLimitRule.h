#pragma once

#include <chrono>
#include <cstddef>

namespace ruvia {

// Per-worker, per-address fixed-window rule. maxRequests == 0 disables the
// app-level rule. Every worker owns an independent allocation-free key table, so
// the admitted process-wide total depends on how connections are distributed
// across workers. failClosed selects the result when that worker cannot track a
// new key.
struct RateLimitRule final {
    std::size_t maxRequests{0};
    std::chrono::milliseconds window{std::chrono::seconds(1)};
    bool failClosed{true};
};

namespace detail {

[[nodiscard]] constexpr RateLimitRule normalizeRateLimitRule(RateLimitRule rule) noexcept {
    if (rule.window <= std::chrono::milliseconds::zero()) {
        rule.window = std::chrono::milliseconds(1);
    }
    return rule;
}

}  // namespace detail

}  // namespace ruvia
