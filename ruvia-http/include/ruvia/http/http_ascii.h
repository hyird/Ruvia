#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia {

[[nodiscard]] inline unsigned char http_ascii_to_lower(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] inline bool http_ascii_equals_ignore_case(
    std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (http_ascii_to_lower(static_cast<unsigned char>(left[i])) !=
            http_ascii_to_lower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace ruvia
