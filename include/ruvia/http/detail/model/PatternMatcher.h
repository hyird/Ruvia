#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/model/PatternCompiler.h"

namespace ruvia::detail::model {

// Upper bound on backtracking steps for a single pattern match. The matcher is a
// greedy backtracking engine, so a pattern with adjacent unanchored quantifiers
// (e.g. "[0-9]*[0-9]*...") could backtrack combinatorially over hostile input.
// Patterns are compile-time constants and the input is validated per request, so
// this caps the worst case as a ReDoS defence-in-depth: exceeding it fails the
// match (the field is rejected). A generous bound no real match approaches.
inline constexpr std::size_t kMaxPatternMatchSteps = 1'000'000;

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
    std::size_t valueIndex,
    std::size_t& budget) noexcept {
    if (budget == 0) {
        return false;  // step budget exhausted -> bound catastrophic backtracking
    }
    --budget;
    if (atomIndex == plan.count) {
        return valueIndex == value.size();
    }

    const auto& atom = plan.atoms[atomIndex];
    const std::size_t minCount =
        atom.quantifier == PatternQuantifier::kOne || atom.quantifier == PatternQuantifier::kOneOrMore ? 1 : 0;
    // Bound the greedy scan at the most this quantifier can consume. kOne/kZeroOrOne
    // take at most one character, so scanning the whole matching run and discarding
    // all but one is wasted O(L) work -- and it is repeated at every backtrack
    // position, an O(n^2) ReDoS on shapes like "^a*a$" that the per-recursion step
    // budget never charges for (each full rescan is a single call). Capping the scan
    // makes each fixed atom O(1), so per-call scan cost stays within the
    // budget-charged recursion; the variable quantifiers already spawn ~L recursions
    // for an L-char scan and so remain bounded.
    const std::size_t scanLimit =
        atom.quantifier == PatternQuantifier::kOne ||
                atom.quantifier == PatternQuantifier::kZeroOrOne
            ? std::size_t{1}
            : value.size();
    std::size_t maxCount = 0;
    while (maxCount < scanLimit &&
           valueIndex + maxCount < value.size() &&
           matchPatternAtom(pattern, atom, value[valueIndex + maxCount])) {
        ++maxCount;
    }

    if (maxCount < minCount) {
        return false;
    }

    for (std::size_t count = maxCount + 1; count-- > minCount;) {
        if (matchPatternPlanFrom(plan, pattern, value, atomIndex + 1, valueIndex + count, budget)) {
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
    std::size_t budget = kMaxPatternMatchSteps;
    return matchPatternPlanFrom(plan, Pattern.view(), value, 0, 0, budget);
}

}  // namespace ruvia::detail::model
