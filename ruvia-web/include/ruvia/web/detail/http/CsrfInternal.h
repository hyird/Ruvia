#pragma once

#include <span>
#include <string_view>

#include "ruvia/core/detail/ConstantTime.h"

namespace ruvia::detail {

// Fills `buffer` (which must hold at least 48 bytes) with a cryptographically
// random hex token and returns a view of it, or an empty view on RNG failure.
[[nodiscard]] std::string_view generateCsrfToken(std::span<char> buffer) noexcept;

// Length-checked constant-time compare of the double-submit CSRF token; see
// constantTimeBytesEqual for the timing-safety rationale.
[[nodiscard]] inline bool csrfTokensEqual(std::string_view left, std::string_view right) noexcept {
    return constantTimeBytesEqual(left, right);
}

}  // namespace ruvia::detail
