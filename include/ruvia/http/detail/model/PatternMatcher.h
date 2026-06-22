#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/model/PatternCompiler.h"

namespace ruvia::detail::model {

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

}  // namespace ruvia::detail::model
