#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/web/detail/model/pattern/pattern_types.h"
#include "ruvia/web/model_types.h"

namespace ruvia::detail::model {

inline constexpr std::size_t max_pattern_bytes = 256;

template <std::size_t capacity>
[[nodiscard]] constexpr bool append_pattern_atom(std::string_view pattern, std::size_t end,
    std::size_t& cursor_value, pattern_plan<capacity>& plan) noexcept {
    if (cursor_value >= end || plan.count_ >= capacity) {
        return false;
    }

    pattern_atom atom{};
    const char c = pattern[cursor_value];
    if (c == '[') {
        ++cursor_value;
        atom.kind_ = pattern_atom_kind::class_value;
        if (cursor_value < end && pattern[cursor_value] == '^') {
            atom.negate_class_ = true;
            ++cursor_value;
        }
        atom.class_begin_ = cursor_value;
        for (; cursor_value < end; ++cursor_value) {
            if (pattern[cursor_value] == '\\') {
                ++cursor_value;
                continue;
            }
            if (pattern[cursor_value] == ']') {
                atom.class_end_ = cursor_value++;
                plan.atoms_[plan.count_++] = atom;
                return true;
            }
        }
        return false;
    }

    if (c == '\\') {
        if (cursor_value + 1 >= end) {
            return false;
        }
        const char escaped = pattern[++cursor_value];
        switch (escaped) {
            case 'd':
                atom.kind_ = pattern_atom_kind::digit;
                break;
            case 'w':
                atom.kind_ = pattern_atom_kind::word;
                break;
            case 's':
                atom.kind_ = pattern_atom_kind::space;
                break;
            default:
                atom.kind_ = pattern_atom_kind::literal;
                atom.literal_ = escaped;
                break;
        }
        ++cursor_value;
        plan.atoms_[plan.count_++] = atom;
        return true;
    }

    if (is_pattern_meta(c)) {
        if (c != '.') {
            return false;
        }
        atom.kind_ = pattern_atom_kind::any;
    } else {
        atom.kind_ = pattern_atom_kind::literal;
        atom.literal_ = c;
    }
    ++cursor_value;
    plan.atoms_[plan.count_++] = atom;
    return true;
}

template <std::size_t capacity>
[[nodiscard]] constexpr pattern_plan<capacity> compile_pattern_plan(
    std::string_view pattern) noexcept {
    pattern_plan<capacity> plan{};
    if (pattern.size() > max_pattern_bytes || pattern.size() < 2 ||
        pattern.front() != '^' || pattern.back() != '$') {
        return plan;
    }

    const std::size_t pattern_end = pattern.size() - 1;
    std::size_t cursor_value = 1;
    while (cursor_value < pattern_end) {
        const std::size_t index = plan.count_;
        if (!append_pattern_atom(pattern, pattern_end, cursor_value, plan)) {
            return {};
        }

        if (cursor_value < pattern_end &&
            (pattern[cursor_value] == '*' || pattern[cursor_value] == '+' || pattern[cursor_value] == '?')) {
            switch (pattern[cursor_value++]) {
                case '*':
                    plan.atoms_[index].quantifier_ = pattern_quantifier::zero_or_more;
                    break;
                case '+':
                    plan.atoms_[index].quantifier_ = pattern_quantifier::one_or_more;
                    break;
                case '?':
                    plan.atoms_[index].quantifier_ = pattern_quantifier::zero_or_one;
                    break;
                default:
                    return {};
            }
        }
    }

    plan.valid_ = true;
    return plan;
}

template <fixed_string pattern>
struct compiled_pattern_plan final {
    static constexpr std::size_t capacity =
        pattern.view().size() < max_pattern_bytes ? pattern.view().size() : max_pattern_bytes;
    static constexpr auto value = compile_pattern_plan<capacity>(pattern.view());
    static_assert(value.valid_,
        "RUVIA_PATTERN supports only anchored lightweight full-match patterns. "
        "Use RUVIA_REGEX for full std::regex syntax or RUVIA_CUSTOM for a custom hot-path "
        "matcher.");
};

}  // namespace ruvia::detail::model
