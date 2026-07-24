#pragma once

#include <array>
#include <cstddef>

namespace ruvia::detail::model {

enum class PatternAtomKind : unsigned char { kLiteral, kAny, kDigit, kWord, kSpace, kClass };

enum class PatternQuantifier : unsigned char { kOne, kZeroOrOne, kZeroOrMore, kOneOrMore };

struct PatternAtom final {
    PatternAtomKind kind{PatternAtomKind::kLiteral};
    PatternQuantifier quantifier{PatternQuantifier::kOne};
    char literal{'\0'};
    std::size_t classBegin{0};
    std::size_t classEnd{0};
    bool negateClass{false};
};

template <std::size_t Capacity>
struct PatternPlan final {
    bool valid{false};
    std::size_t count{0};
    std::array<PatternAtom, Capacity> atoms{};
};

[[nodiscard]] constexpr bool isPatternMeta(char c) noexcept {
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

[[nodiscard]] constexpr bool isPatternDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr bool isPatternWord(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] constexpr bool isPatternSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

}  // namespace ruvia::detail::model
