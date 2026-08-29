#pragma once

#include <string_view>

namespace ruvia::detail {

inline void skipJsonWhitespace(std::string_view& input) noexcept {
    while (!input.empty()) {
        const char c = input.front();
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return;
        }
        input.remove_prefix(1);
    }
}

[[nodiscard]] inline bool consumeJsonChar(std::string_view& input, char expected) noexcept {
    skipJsonWhitespace(input);
    if (input.empty() || input.front() != expected) {
        return false;
    }
    input.remove_prefix(1);
    return true;
}

[[nodiscard]] inline bool consumeJsonLiteral(
    std::string_view& input, std::string_view literal) noexcept {
    skipJsonWhitespace(input);
    if (!input.starts_with(literal)) {
        return false;
    }
    input.remove_prefix(literal.size());
    return true;
}

}  // namespace ruvia::detail
