#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/web/fixed_string.h"

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

// Decode escaped atoms before recognizing a range; a final '-' is a literal,
// and only single literal atoms may be range endpoints.
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

constexpr void insert_pattern_member(pattern_byte_set& bytes, const pattern_class_member& member) noexcept {
    for (unsigned byte = 0; byte < 256; ++byte) {
        const auto value = static_cast<char>(byte);
        bool matches = false;
        switch (member.kind_) {
            case pattern_atom_kind::literal:
                matches = byte >= member.first_ && byte <= member.last_;
                break;
            case pattern_atom_kind::digit:
                matches = is_pattern_digit(value);
                break;
            case pattern_atom_kind::word:
                matches = is_pattern_word(value);
                break;
            case pattern_atom_kind::space:
                matches = is_pattern_space(value);
                break;
        }
        if (matches) {
            bytes.insert(static_cast<unsigned char>(byte));
        }
    }
}

// Folds the class body in [begin, end) into `bytes`. Negation is applied by
// the caller: a leading '[^' is stripped before the body, so any '^' here is an
// ordinary member (e.g. "[^^]" excludes only the caret).
[[nodiscard]] constexpr bool compile_pattern_class(std::string_view pattern, std::size_t begin,
    std::size_t end, pattern_byte_set& bytes) noexcept {
    if (begin >= end) {
        return false;
    }
    for (std::size_t cursor = begin; cursor < end;) {
        pattern_class_member member;
        if (!consume_pattern_class_member(pattern, end, cursor, member)) {
            return false;
        }
        insert_pattern_member(bytes, member);
    }
    return true;
}

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
        bool negate = false;
        if (cursor_value < end && pattern[cursor_value] == '^') {
            negate = true;
            ++cursor_value;
        }
        const auto class_begin = cursor_value;
        for (; cursor_value < end; ++cursor_value) {
            if (pattern[cursor_value] == '\\') {
                ++cursor_value;
                continue;
            }
            if (pattern[cursor_value] == ']') {
                const auto class_end = cursor_value++;
                if (!compile_pattern_class(pattern, class_begin, class_end, atom.bytes_)) {
                    return false;
                }
                if (negate) {
                    atom.bytes_.invert();
                }
                plan.atoms_[plan.count_++] = atom;
                return true;
            }
        }
        return false;
    }

    pattern_class_member member;
    if (c == '\\') {
        if (cursor_value + 1 >= end) {
            return false;
        }
        const char escaped = pattern[++cursor_value];
        if (!decode_pattern_escape(escaped, member.kind_)) {
            return false;
        }
        member.first_ = static_cast<unsigned char>(escaped);
        member.last_ = member.first_;
        insert_pattern_member(atom.bytes_, member);
    } else if (c == '.') {
        atom.bytes_.insert(static_cast<unsigned char>('\n'));
        atom.bytes_.invert();
    } else if (is_pattern_meta(c)) {
        return false;
    } else {
        atom.bytes_.insert(static_cast<unsigned char>(c));
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
        "RUVIA_PATTERN supports only anchored patterns using literals, '.', character classes, \\d, \\w, \\s, and '*', '+', '?'. "
        "Escaped letters and digits other than \\d, \\w, \\s (such as \\n or \\D) are unsupported. "
        "Ranges require ordered single-character endpoints. Use RUVIA_CUSTOM for other matching logic.");
};

// Upper bound on matcher work for one pattern match. It charges recursive
// states and each input byte inspected by a quantifier scan; atoms match in
// O(1) against their compiled byte sets. Exhaustion fails the match closed.
inline constexpr std::size_t max_pattern_match_steps = 1'000'000;

template <std::size_t capacity>
[[nodiscard]] constexpr bool match_pattern_plan_from(const pattern_plan<capacity>& plan,
    std::string_view value, std::size_t atom_index, std::size_t value_index, std::size_t& budget) noexcept {
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
        if (!atom.bytes_.contains(value[value_index + max_count])) {
            break;
        }
        ++max_count;
    }

    if (max_count < min_count) {
        return false;
    }

    for (std::size_t count = max_count + 1; count-- > min_count;) {
        if (match_pattern_plan_from(plan, value, atom_index + 1, value_index + count, budget)) {
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
    std::size_t budget = max_pattern_match_steps;
    return match_pattern_plan_from(compiled_pattern_plan<pattern>::value, value, 0, 0, budget);
}

}  // namespace ruvia::detail::model
