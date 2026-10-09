#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace ruvia {

// Each worker preallocates this many fixed-window key entries when either the
// app-wide default rule or a route-specific rate-limit middleware is present.
inline constexpr std::size_t default_rate_limit_capacity_per_worker = 8192;

enum class rate_limit_overflow_policy : std::uint8_t {
    deny,
    allow,
};

// Per-worker, per-address fixed-window rule. Configuration remains a plain
// value; each owner validates it once before publishing runtime state.
struct rate_limit_rule final {
    std::size_t max_requests_{0};
    std::chrono::milliseconds window_{0};
    rate_limit_overflow_policy overflow_policy_{rate_limit_overflow_policy::deny};
};

struct rate_limit_config final {
    rate_limit_rule rule_{};
    std::size_t capacity_per_worker_{default_rate_limit_capacity_per_worker};
};

namespace detail {

inline constexpr void validate_rate_limit_rule(const rate_limit_rule& rule) {
    if (rule.max_requests_ == 0) {
        throw std::invalid_argument("rate limit max requests must be greater than zero");
    }
    if (rule.window_.count() <= 0) {
        throw std::invalid_argument("rate limit window must be greater than zero");
    }
    if (rule.overflow_policy_ != rate_limit_overflow_policy::deny &&
        rule.overflow_policy_ != rate_limit_overflow_policy::allow) {
        throw std::invalid_argument("rate limit overflow policy is invalid");
    }
}

}  // namespace detail

}  // namespace ruvia
