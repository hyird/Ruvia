#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/detail/model/pattern/PatternTypes.h"

namespace ruvia::detail::model {

template <std::size_t Capacity>
[[nodiscard]] constexpr bool appendPatternAtom(std::string_view pattern, std::size_t end, std::size_t& cursor, PatternPlan<Capacity>& plan) noexcept {
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

        if (cursor < patternEnd && (pattern[cursor] == '*' || pattern[cursor] == '+' || pattern[cursor] == '?')) {
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
        "Use RUVIA_REGEX for full std::regex syntax or RUVIA_CUSTOM for a custom hot-path "
        "matcher.");
};

}  // namespace ruvia::detail::model
