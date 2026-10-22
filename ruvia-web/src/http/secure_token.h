#pragma once

#include <optional>
#include <span>
#include <string_view>

namespace ruvia::detail {

// Fills `buffer` (which must hold at least 48 bytes) with a cryptographically
// random hex token. Failure returns std::nullopt.
[[nodiscard]] std::optional<std::string_view> generate_secure_token(std::span<char> buffer) noexcept;

}  // namespace ruvia::detail
