#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/core/stop_token.h"

namespace ruvia {

// Common cancellation policy for lazy outbound operations. A configured
// timeout is an end-to-end bound beginning when the operation starts and must
// be greater than zero.
struct operation_options final {
    std::optional<std::chrono::milliseconds> timeout_{};
    stop_token stop_token_{};
};

namespace detail {

inline void validate_operation_options(const operation_options& options) {
    if (options.timeout_.has_value() && options.timeout_->count() <= 0) {
        throw std::invalid_argument("operation timeout must be greater than zero");
    }
}

[[nodiscard]] inline operation_options merge_operation_options(
    const operation_options& base, operation_options overrides) {
    operation_options merged = base;
    if (overrides.timeout_.has_value() &&
        (!merged.timeout_.has_value() || *overrides.timeout_ < *merged.timeout_)) {
        merged.timeout_ = overrides.timeout_;
    }
    merged.stop_token_ = combine_stop_tokens(base.stop_token_, std::move(overrides.stop_token_));
    return merged;
}

}  // namespace detail

}  // namespace ruvia
