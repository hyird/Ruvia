#pragma once

#include <string_view>

namespace ruvia::detail {

inline void skip_json_whitespace(std::string_view& input) noexcept {
    while (!input.empty()) {
        const char c = input.front();
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return;
        }
        input.remove_prefix(1);
    }
}

[[nodiscard]] inline bool consume_json_char(std::string_view& input, char expected) noexcept {
    skip_json_whitespace(input);
    if (input.empty() || input.front() != expected) {
        return false;
    }
    input.remove_prefix(1);
    return true;
}

[[nodiscard]] inline bool consume_json_literal(
    std::string_view& input, std::string_view literal) noexcept {
    skip_json_whitespace(input);
    if (!input.starts_with(literal)) {
        return false;
    }
    input.remove_prefix(literal.size());
    return true;
}

}  // namespace ruvia::detail
