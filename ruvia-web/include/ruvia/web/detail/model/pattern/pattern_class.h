#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/web/detail/model/pattern/pattern_types.h"

namespace ruvia::detail::model {

struct pattern_class_member final {
    pattern_atom_kind kind_{pattern_atom_kind::literal};
    unsigned char first_{0};
    unsigned char last_{0};
};

// Only \d, \w and \s are escape classes. Other ASCII letters and digits are
// rejected rather than read as literals, because regex escapes such as \r, \n,
// \D or \S would otherwise silently change meaning. Remaining escapes are literal.
[[nodiscard]] constexpr bool decode_pattern_escape(char escape, pattern_atom_kind& kind) noexcept {
    switch (escape) {
        case 'd':
            kind = pattern_atom_kind::digit;
            return true;
        case 'w':
            kind = pattern_atom_kind::word;
            return true;
        case 's':
            kind = pattern_atom_kind::space;
            return true;
        default:
            kind = pattern_atom_kind::literal;
            return escape == '_' || !is_pattern_word(escape);
    }
}

[[nodiscard]] constexpr bool consume_pattern_class_atom(std::string_view pattern, std::size_t end,
    std::size_t& cursor, pattern_class_member& member) noexcept {
    if (cursor >= end) {
        return false;
    }
    char value = pattern[cursor++];
    member.kind_ = pattern_atom_kind::literal;
    if (value == '\\') {
        if (cursor >= end) {
            return false;
        }
        value = pattern[cursor++];
        if (!decode_pattern_escape(value, member.kind_)) {
            return false;
        }
    }
    member.first_ = static_cast<unsigned char>(value);
    member.last_ = member.first_;
    return true;
}

// The compiler and matcher share this grammar. Decode escaped atoms before
// recognizing a range; a final '-' is a literal, and only single literal atoms
// may be range endpoints.
[[nodiscard]] constexpr bool consume_pattern_class_member(std::string_view pattern, std::size_t end,
    std::size_t& cursor, pattern_class_member& member) noexcept {
    if (!consume_pattern_class_atom(pattern, end, cursor, member)) {
        return false;
    }
    if (cursor + 1 < end && pattern[cursor] == '-') {
        ++cursor;
        pattern_class_member last;
        if (!consume_pattern_class_atom(pattern, end, cursor, last) ||
            member.kind_ != pattern_atom_kind::literal || last.kind_ != pattern_atom_kind::literal ||
            member.first_ > last.first_) {
            return false;
        }
        member.last_ = last.first_;
    }
    return true;
}

[[nodiscard]] constexpr bool valid_pattern_class(std::string_view pattern, std::size_t begin,
    std::size_t end) noexcept {
    if (begin >= end) {
        return false;
    }
    for (std::size_t cursor = begin; cursor < end;) {
        pattern_class_member member;
        if (!consume_pattern_class_member(pattern, end, cursor, member)) {
            return false;
        }
    }
    return true;
}

}  // namespace ruvia::detail::model
