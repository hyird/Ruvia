#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/web/detail/model/pattern/pattern_compiler.h"

namespace ruvia::detail::model {

// Upper bound on matcher work for one pattern match. It charges recursive states,
// each input byte inspected by a quantifier scan, and each pattern byte inspected
// while matching a character class. Exhaustion fails the match closed.
inline constexpr std::size_t max_pattern_match_steps = 1'000'000;

[[nodiscard]] constexpr bool match_pattern_escape(char escape, char value) noexcept {
    switch (escape) {
        case 'd':
            return is_pattern_digit(value);
        case 'w':
            return is_pattern_word(value);
        case 's':
            return is_pattern_space(value);
        default:
            return value == escape;
    }
}

// Matches `value` against the class body in [begin, end). Negation is NOT handled
// here: a leading '[^' is stripped by the compiler, which records it on the atom's
// negate_class_ flag (applied once in match_pattern_atom). Treating a leading '^' as
// negation here as well would double-negate a class whose first literal member is
// itself a caret (e.g. "[^^]"), so any '^' in the body is an ordinary member.
[[nodiscard]] constexpr bool match_pattern_class(std::string_view pattern, std::size_t begin,
    std::size_t end, char value, std::size_t& budget) noexcept {
    bool matched = false;
    for (std::size_t i = begin; i < end;) {
        if (budget == 0) {
            return false;
        }
        --budget;
        char first = pattern[i++];
        if (first == '\\') {
            if (i >= end || budget == 0) {
                return false;
            }
            --budget;
            // The class-member escape shares the atom escape semantics
            // (\d, \w, \s, or a literal) -- one owner, match_pattern_escape.
            matched = matched || match_pattern_escape(pattern[i++], value);
            continue;
        }

        if (i + 1 < end && pattern[i] == '-') {
            if (budget < 2) {
                return false;
            }
            budget -= 2;
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

// Keep the helper's direct test/use form; the matcher itself always supplies its
// shared budget through the overload above.
[[nodiscard]] constexpr bool match_pattern_class(
    std::string_view pattern, std::size_t begin, std::size_t end, char value) noexcept {
    std::size_t budget = max_pattern_match_steps;
    return match_pattern_class(pattern, begin, end, value, budget);
}

[[nodiscard]] constexpr bool match_pattern_atom(std::string_view pattern,
    const pattern_atom& atom, char value, std::size_t& budget) noexcept {
    switch (atom.kind_) {
        case pattern_atom_kind::literal:
            return value == atom.literal_;
        case pattern_atom_kind::any:
            return value != '\n';
        case pattern_atom_kind::digit:
            return is_pattern_digit(value);
        case pattern_atom_kind::word:
            return is_pattern_word(value);
        case pattern_atom_kind::space:
            return is_pattern_space(value);
        case pattern_atom_kind::class_value: {
            const bool matched = match_pattern_class(
                pattern, atom.class_begin_, atom.class_end_, value, budget);
            return atom.negate_class_ ? !matched : matched;
        }
    }
    return false;
}

template <std::size_t capacity>
[[nodiscard]] constexpr bool match_pattern_plan_from(const pattern_plan<capacity>& plan,
    std::string_view pattern, std::string_view value, std::size_t atom_index, std::size_t value_index,
    std::size_t& budget) noexcept {
    if (budget == 0) {
        return false;  // step budget exhausted -> bound catastrophic backtracking
    }
    --budget;
    if (atom_index == plan.count_) {
        return value_index == value.size();
    }

    const auto& atom = plan.atoms_[atom_index];
    const std::size_t min_count = atom.quantifier_ == pattern_quantifier::one ||
                                          atom.quantifier_ == pattern_quantifier::one_or_more
                                      ? 1
                                      : 0;
    // Bound the greedy scan at the most this quantifier can consume. one/zero_or_one
    // take at most one character, so scanning the whole matching run and discarding
    // all but one is wasted O(L) work -- and it is repeated at every backtrack
    // position, an O(n^2) ReDoS on shapes like "^a*a$" that the per-recursion step
    // budget never charges for (each full rescan is a single call). Capping the scan
    // makes each fixed atom O(1), so per-call scan cost stays within the
    // budget-charged recursion; the variable quantifiers already spawn ~L recursions
    // for an L-char scan and so remain bounded.
    const std::size_t scan_limit = atom.quantifier_ == pattern_quantifier::one ||
                                           atom.quantifier_ == pattern_quantifier::zero_or_one
                                       ? std::size_t{1}
                                       : value.size();
    std::size_t max_count = 0;
    while (max_count < scan_limit && value_index + max_count < value.size()) {
        if (budget == 0) {
            return false;
        }
        --budget;
        if (!match_pattern_atom(pattern, atom, value[value_index + max_count], budget)) {
            if (budget == 0) {
                return false;
            }
            break;
        }
        ++max_count;
    }

    if (max_count < min_count) {
        return false;
    }

    for (std::size_t count = max_count + 1; count-- > min_count;) {
        if (match_pattern_plan_from(plan, pattern, value, atom_index + 1, value_index + count, budget)) {
            return true;
        }
        if (count == 0) {
            break;
        }
    }
    return false;
}

template <fixed_string pattern>
[[nodiscard]] constexpr bool match_pattern_plan(std::string_view value) noexcept {
    constexpr auto plan = compiled_pattern_plan<pattern>::value;
    std::size_t budget = max_pattern_match_steps;
    return match_pattern_plan_from(plan, pattern.view(), value, 0, 0, budget);
}

}  // namespace ruvia::detail::model
