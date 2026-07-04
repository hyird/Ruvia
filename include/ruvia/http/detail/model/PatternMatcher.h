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

// Matches `value` against the class body in [begin, end). Negation is NOT handled
// here: a leading '[^' is stripped by the compiler, which records it on the atom's
// negateClass flag (applied once in matchPatternAtom). Treating a leading '^' as
// negation here as well would double-negate a class whose first literal member is
// itself a caret (e.g. "[^^]"), so any '^' in the body is an ordinary member.
[[nodiscard]] constexpr bool matchPatternClass(
    std::string_view pattern,
    std::size_t begin,
    std::size_t end,
    char value) noexcept {
    bool matched = false;
    for (std::size_t i = begin; i < end;) {
        char first = pattern[i++];
        if (first == '\\') {
            if (i >= end) {
                return false;
            }
            // The class-member escape shares the atom escape semantics
            // (\d, \w, \s, or a literal) -- one owner, matchPatternEscape.
            matched = matched || matchPatternEscape(pattern[i++], value);
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

    return matched;
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
