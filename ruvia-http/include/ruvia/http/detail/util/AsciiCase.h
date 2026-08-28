#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] inline unsigned char httpAsciiToLower(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] inline bool httpAsciiEqualsIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (httpAsciiToLower(static_cast<unsigned char>(left[i])) != httpAsciiToLower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace ruvia::detail
