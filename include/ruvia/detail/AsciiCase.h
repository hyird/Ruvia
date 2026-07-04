#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia::detail {

// Single owner of ASCII-only case handling, shared across layers (HTTP header
// parsing, the model media-type matcher, the Redis reply helpers, dotenv
// parsing, ...). Dependency-free and public-header-safe so both include/ and
// src/ callers can reuse it without inverting the layering. ASCII-only by
// design: HTTP tokens, media types, encoding names, and env keys are all ASCII,
// and std::tolower would be locale-dependent.
[[nodiscard]] inline unsigned char asciiToLower(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] inline bool asciiEqualsIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (asciiToLower(static_cast<unsigned char>(left[i])) !=
            asciiToLower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace ruvia::detail
