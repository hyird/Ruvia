#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/stop_token.h"

namespace ruvia {

// The single per-operation input of every cold wait and outbound operation.
// timeout_ bounds the operation end to end: a relative limit starts when the
// operation starts, an absolute deadline is used as given.
struct operation_options final {
    std::optional<operation_timeout> timeout_{};
    stop_token stop_token_{};
};

// Unspecified fields inherit. A supplied token replaces the user cancellation
// state; callers needing accumulation own a stop_group until their work joins.
// A supplied timeout may tighten but never relax the inherited bound: two
// relative limits keep the shorter, two deadlines keep the earlier, and a
// mixed pair resolves the relative limit against the merge time and keeps the
// earlier deadline.
[[nodiscard]] inline operation_options override_operation_options(
    const operation_options& base, operation_options overrides) {
    operation_options merged = base;
    if (overrides.timeout_.has_value()) {
        if (!merged.timeout_.has_value()) {
            merged.timeout_ = overrides.timeout_;
        } else {
            const auto& current = *merged.timeout_;
            const auto& candidate = *overrides.timeout_;
            const auto current_duration = current.duration();
            const auto candidate_duration = candidate.duration();
            if (current_duration.has_value() && candidate_duration.has_value()) {
                if (*candidate_duration < *current_duration) {
                    merged.timeout_ = candidate;
                }
            } else {
                const auto now = operation_timeout::clock_type::now();
                const auto current_deadline = current.deadline_from(now);
                const auto candidate_deadline = candidate.deadline_from(now);
                merged.timeout_ = operation_timeout(std::min(current_deadline, candidate_deadline));
            }
        }
    }
    if (overrides.stop_token_.has_state()) {
        merged.stop_token_ = std::move(overrides.stop_token_);
    }
    return merged;
}

}  // namespace ruvia
