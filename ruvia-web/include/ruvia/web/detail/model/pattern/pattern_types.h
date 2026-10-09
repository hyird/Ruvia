#pragma once

#include <array>
#include <cstddef>

namespace ruvia::detail::model {

enum class pattern_atom_kind : unsigned char { literal,
    any,
    digit,
    word,
    space,
    class_value };

enum class pattern_quantifier : unsigned char { one,
    zero_or_one,
    zero_or_more,
    one_or_more };

struct pattern_atom final {
    pattern_atom_kind kind_{pattern_atom_kind::literal};
    pattern_quantifier quantifier_{pattern_quantifier::one};
    char literal_{'\0'};
    std::size_t class_begin_{0};
    std::size_t class_end_{0};
    bool negate_class_{false};
};

template <std::size_t capacity>
struct pattern_plan final {
    bool valid_{false};
    std::size_t count_{0};
    std::array<pattern_atom, capacity> atoms_{};
};

[[nodiscard]] constexpr bool is_pattern_meta(char c) noexcept {
    switch (c) {
        case '^':
        case '$':
        case '[':
        case ']':
        case '(':
        case ')':
        case '{':
        case '}':
        case '|':
        case '+':
        case '*':
        case '?':
        case '.':
        case '\\':
            return true;
        default:
            return false;
    }
}

[[nodiscard]] constexpr bool is_pattern_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr bool is_pattern_word(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] constexpr bool is_pattern_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

}  // namespace ruvia::detail::model
