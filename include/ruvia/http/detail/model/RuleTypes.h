#pragma once

#include <array>
#include <regex>
#include <string>
#include <string_view>

namespace ruvia::detail::model {

struct Required final {
    using RuviaValidationRuleMarker = void;

    std::string_view message{"is required"};
};

struct Min final {
    using RuviaValidationRuleMarker = void;

    long double value{0};
    std::string_view message{"is too small"};
};

struct Max final {
    using RuviaValidationRuleMarker = void;

    long double value{0};
    std::string_view message{"is too big"};
};

template <FixedString... Values>
struct OneOf final {
    using RuviaValidationRuleMarker = void;

    std::string_view message{"is not allowed"};
};

struct Email final {
    using RuviaValidationRuleMarker = void;

    std::string_view message{"must be a valid email"};
};

template <typename PredicateT>
struct Custom final {
    using RuviaValidationRuleMarker = void;

    std::string_view message{"is invalid"};
    PredicateT predicate;
};

template <typename PredicateT>
struct Match final {
    using RuviaValidationRuleMarker = void;

    std::string_view message{"does not match"};
    PredicateT predicate;
};

template <typename ValidatorT>
struct Nested final {
    using RuviaValidationRuleMarker = void;
};

template <typename ValidatorT>
struct Each final {
    using RuviaValidationRuleMarker = void;
};

template <typename ValueT>
struct Default final {
    using RuviaDefaultRuleMarker = void;
    using RuviaModelOptionMarker = void;

    ValueT value;
};

template <typename ValueT>
Default(ValueT) -> Default<ValueT>;

struct OmitEmpty final {
    using RuviaModelOptionMarker = void;
};

struct EmitNull final {
    using RuviaModelOptionMarker = void;
};

enum class PatternMatchResult {
    kUnsupported,
    kNoMatch,
    kMatch
};

enum class PatternAtomKind : unsigned char {
    kLiteral,
    kAny,
    kDigit,
    kWord,
    kSpace,
    kClass
};

enum class PatternQuantifier : unsigned char {
    kOne,
    kZeroOrOne,
    kZeroOrMore,
    kOneOrMore
};

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
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] constexpr bool isPatternSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

[[nodiscard]] constexpr bool matchPatternEscape(char escape, char value) noexcept {
    switch (escape) {
    case 'd':
        return isPatternDigit(value);
    case 'w':
        return isPatternWord(value);
    case 's':
        return isPatternSpace(value);
    default:
        return value == escape;
    }
}

constexpr void matchPatternClassEscape(char escape, char value, bool& matched) noexcept {
    switch (escape) {
    case 'd':
        matched = isPatternDigit(value);
        return;
    case 'w':
        matched = isPatternWord(value);
        return;
    case 's':
        matched = isPatternSpace(value);
        return;
    default:
        matched = value == escape;
        return;
    }
}

[[nodiscard]] constexpr bool matchPatternClass(
    std::string_view pattern,
    std::size_t begin,
    std::size_t end,
    char value) noexcept {
    bool negate = false;
    if (begin < end && pattern[begin] == '^') {
        negate = true;
        ++begin;
    }

    bool matched = false;
    for (std::size_t i = begin; i < end;) {
        char first = pattern[i++];
        if (first == '\\') {
            if (i >= end) {
                return false;
            }
            bool escapedMatched = false;
            matchPatternClassEscape(pattern[i++], value, escapedMatched);
            matched = matched || escapedMatched;
            continue;
        }

        if (i + 1 < end && pattern[i] == '-') {
            char last = pattern[i + 1];
            if (last == '\\' || first > last) {
                return false;
            }
            matched = matched || (value >= first && value <= last);
            i += 2;
            continue;
        }

        matched = matched || value == first;
    }

    return negate ? !matched : matched;
}

[[nodiscard]] constexpr bool matchPatternAtom(
    std::string_view pattern,
    const PatternAtom& atom,
    char value) noexcept {
    switch (atom.kind) {
    case PatternAtomKind::kLiteral:
        return value == atom.literal;
    case PatternAtomKind::kAny:
        return value != '\n';
    case PatternAtomKind::kDigit:
        return isPatternDigit(value);
    case PatternAtomKind::kWord:
        return isPatternWord(value);
    case PatternAtomKind::kSpace:
        return isPatternSpace(value);
    case PatternAtomKind::kClass: {
        const bool matched = matchPatternClass(pattern, atom.classBegin, atom.classEnd, value);
        return atom.negateClass ? !matched : matched;
    }
    }
    return false;
}

template <std::size_t Capacity>
[[nodiscard]] constexpr bool appendPatternAtom(
    std::string_view pattern,
    std::size_t end,
    std::size_t& cursor,
    PatternPlan<Capacity>& plan) noexcept {
    if (cursor >= end || plan.count >= Capacity) {
        return false;
    }

    PatternAtom atom{};
    const char c = pattern[cursor];
    if (c == '[') {
        ++cursor;
        atom.kind = PatternAtomKind::kClass;
        if (cursor < end && pattern[cursor] == '^') {
            atom.negateClass = true;
            ++cursor;
        }
        atom.classBegin = cursor;
        for (; cursor < end; ++cursor) {
            if (pattern[cursor] == '\\') {
                ++cursor;
                continue;
            }
            if (pattern[cursor] == ']') {
                atom.classEnd = cursor++;
                plan.atoms[plan.count++] = atom;
                return true;
            }
        }
        return false;
    }

    if (c == '\\') {
        if (cursor + 1 >= end) {
            return false;
        }
        const char escaped = pattern[++cursor];
        switch (escaped) {
        case 'd':
            atom.kind = PatternAtomKind::kDigit;
            break;
        case 'w':
            atom.kind = PatternAtomKind::kWord;
            break;
        case 's':
            atom.kind = PatternAtomKind::kSpace;
            break;
        default:
            atom.kind = PatternAtomKind::kLiteral;
            atom.literal = escaped;
            break;
        }
        ++cursor;
        plan.atoms[plan.count++] = atom;
        return true;
    }

    if (isPatternMeta(c)) {
        if (c != '.') {
            return false;
        }
        atom.kind = PatternAtomKind::kAny;
    } else {
        atom.kind = PatternAtomKind::kLiteral;
        atom.literal = c;
    }
    ++cursor;
    plan.atoms[plan.count++] = atom;
    return true;
}

template <std::size_t Capacity>
[[nodiscard]] constexpr PatternPlan<Capacity> compilePatternPlan(std::string_view pattern) noexcept {
    PatternPlan<Capacity> plan{};
    if (pattern.size() < 2 || pattern.front() != '^' || pattern.back() != '$') {
        return plan;
    }

    const std::size_t patternEnd = pattern.size() - 1;
    std::size_t cursor = 1;
    while (cursor < patternEnd) {
        const std::size_t index = plan.count;
        if (!appendPatternAtom(pattern, patternEnd, cursor, plan)) {
            return {};
        }

        if (cursor < patternEnd &&
            (pattern[cursor] == '*' || pattern[cursor] == '+' || pattern[cursor] == '?')) {
            switch (pattern[cursor++]) {
            case '*':
                plan.atoms[index].quantifier = PatternQuantifier::kZeroOrMore;
                break;
            case '+':
                plan.atoms[index].quantifier = PatternQuantifier::kOneOrMore;
                break;
            case '?':
                plan.atoms[index].quantifier = PatternQuantifier::kZeroOrOne;
                break;
            default:
                return {};
            }
        }
    }

    plan.valid = true;
    return plan;
}

template <FixedString Pattern>
struct CompiledPatternPlan final {
    static constexpr auto value = compilePatternPlan<Pattern.view().size()>(Pattern.view());
    static_assert(value.valid,
        "RUVIA_PATTERN supports only anchored lightweight full-match patterns. "
        "Use RUVIA_REGEX for full std::regex syntax or RUVIA_MATCH for a custom hot-path matcher.");
};

template <FixedString Pattern>
struct PatternRule final {
    using RuviaValidationRuleMarker = void;

    static constexpr auto plan = CompiledPatternPlan<Pattern>::value;

    std::string_view message{"has invalid format"};
};

template <FixedString Pattern>
struct RegexRule final {
    using RuviaValidationRuleMarker = void;

    std::string_view message{"has invalid format"};
};

template <std::size_t Capacity>
[[nodiscard]] constexpr bool matchPatternPlanFrom(
    const PatternPlan<Capacity>& plan,
    std::string_view pattern,
    std::string_view value,
    std::size_t atomIndex,
    std::size_t valueIndex) noexcept {
    if (atomIndex == plan.count) {
        return valueIndex == value.size();
    }

    const auto& atom = plan.atoms[atomIndex];
    const std::size_t minCount =
        atom.quantifier == PatternQuantifier::kOne || atom.quantifier == PatternQuantifier::kOneOrMore ? 1 : 0;
    std::size_t maxCount = 0;
    while (valueIndex + maxCount < value.size() &&
           matchPatternAtom(pattern, atom, value[valueIndex + maxCount])) {
        ++maxCount;
    }

    if (atom.quantifier == PatternQuantifier::kOne || atom.quantifier == PatternQuantifier::kZeroOrOne) {
        maxCount = maxCount > 1 ? 1 : maxCount;
    }
    if (maxCount < minCount) {
        return false;
    }

    for (std::size_t count = maxCount + 1; count-- > minCount;) {
        if (matchPatternPlanFrom(plan, pattern, value, atomIndex + 1, valueIndex + count)) {
            return true;
        }
        if (count == 0) {
            break;
        }
    }
    return false;
}

template <FixedString Pattern>
[[nodiscard]] constexpr bool matchPatternPlan(std::string_view value) noexcept {
    constexpr auto plan = CompiledPatternPlan<Pattern>::value;
    return matchPatternPlanFrom(plan, Pattern.view(), value, 0, 0);
}

template <FixedString Pattern>
[[nodiscard]] const std::regex& compiledPattern() {
    static const std::regex regex(std::string(Pattern.view()));
    return regex;
}

}  // namespace ruvia::detail::model
