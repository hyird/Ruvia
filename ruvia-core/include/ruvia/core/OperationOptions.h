#pragma once

#include "ruvia/core/StopToken.h"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ruvia {

// Common cancellation policy for lazy outbound operations. A configured
// timeout is an end-to-end bound beginning when the operation starts and must
// be greater than zero.
struct OperationOptions final {
    std::optional<std::chrono::milliseconds> timeout{};
    StopToken stopToken{};
};

namespace detail {

inline void validateOperationOptions(const OperationOptions& options) {
    if (options.timeout.has_value() && options.timeout->count() <= 0) {
        throw std::invalid_argument("operation timeout must be greater than zero");
    }
}

[[nodiscard]] inline OperationOptions mergeOperationOptions(
    const OperationOptions& base,
    OperationOptions overrides) {
    OperationOptions merged = base;
    if (overrides.timeout.has_value() &&
        (!merged.timeout.has_value() || *overrides.timeout < *merged.timeout)) {
        merged.timeout = overrides.timeout;
    }
    merged.stopToken = combineStopTokens(base.stopToken, std::move(overrides.stopToken));
    return merged;
}

}  // namespace detail

}  // namespace ruvia
