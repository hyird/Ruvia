#pragma once

#include <chrono>
#include <optional>

#include "ruvia/core/stop_token.h"

namespace ruvia {

// Common cancellation policy for lazy outbound operations. A configured
// timeout is an end-to-end bound beginning when the operation starts and must
// be greater than zero.
// Handle with_options inherits an unspecified token and replaces it when a
// state is supplied; use a caller-owned stop_group for cumulative cancellation.
// Timeout overrides may tighten, but never relax, the existing bound.
struct operation_options final {
    std::optional<std::chrono::milliseconds> timeout_{};
    stop_token stop_token_{};
};

}  // namespace ruvia
