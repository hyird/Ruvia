#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ruvia::detail::model {

// Kinds a single pattern atom or class member decodes to; compilation folds
// each into the byte set it matches.
enum class pattern_atom_kind : unsigned char { literal,
    digit,
    word,
    space };

enum class pattern_quantifier : unsigned char { one,
    zero_or_one,
    zero_or_more,
    one_or_more };

// The bytes one compiled atom matches, as a 256-bit set.
class pattern_byte_set final {
public:
    constexpr void insert(unsigned char byte) noexcept {
        words_[byte >> 6] |= std::uint64_t{1} << (byte & 63U);
    }

    constexpr void invert() noexcept {
        for (auto& word : words_) {
            word = ~word;
        }
    }

    [[nodiscard]] constexpr bool contains(char value) const noexcept {
        const auto byte = static_cast<unsigned char>(value);
        return ((words_[byte >> 6] >> (byte & 63U)) & 1U) != 0;
    }

private:
    std::array<std::uint64_t, 4> words_{};
};

struct pattern_atom final {
    pattern_byte_set bytes_{};
    pattern_quantifier quantifier_{pattern_quantifier::one};
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
