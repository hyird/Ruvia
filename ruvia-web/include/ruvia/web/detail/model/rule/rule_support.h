#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/http/http_number_format.h"
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
    }  // namespace ruvia::detail::model
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
    if (pattern.size() > max_pattern_bytes || pattern.size() < 2 || pattern.front() != '^' || pattern.back() != '$') {
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
#include "ruvia/web/model.h"

namespace ruvia::detail::model {

template <auto bound, fixed_string error_message>
struct min final {
    static_assert(std::is_arithmetic_v<decltype(bound)> && !std::is_same_v<decltype(bound), bool>,
        "RUVIA_MIN requires a numeric bound");
    using ruvia_validation_rule_marker_type = void;
};

template <auto bound, fixed_string error_message>
struct max final {
    static_assert(std::is_arithmetic_v<decltype(bound)> && !std::is_same_v<decltype(bound), bool>,
        "RUVIA_MAX requires a numeric bound");
    using ruvia_validation_rule_marker_type = void;
};

template <fixed_string error_message, fixed_string... values>
struct one_of final {
    using ruvia_validation_rule_marker_type = void;
};

template <fixed_string error_message>
struct email final {
    using ruvia_validation_rule_marker_type = void;
};

template <auto validation_predicate, fixed_string error_message>
struct custom final {
    using ruvia_validation_rule_marker_type = void;
};

template <auto provider>
struct default_value final {
    using ruvia_default_rule_marker_type = void;
    using ruvia_model_option_marker_type = void;

    // Metadata is stateless. Evaluate only when consuming a missing optional
    // field, never while constructing or moving a model.
    [[nodiscard]] static constexpr decltype(auto) value() {
        return provider();
    }
};

struct nullable final {
    using ruvia_model_option_marker_type = void;
};

struct omit_empty final {
    using ruvia_model_option_marker_type = void;
};

struct emit_null final {
    using ruvia_model_option_marker_type = void;
};

template <fixed_string pattern, fixed_string error_message>
struct pattern_rule final {
    using ruvia_validation_rule_marker_type = void;

    static_assert(compiled_pattern_plan<pattern>::value.valid_, "RUVIA_PATTERN requires a valid pattern");
};

template <typename t_type>
[[nodiscard]] std::size_t model_size(const t_type& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view().size();
    } else {
        return value.size();
    }
}

template <typename t_type>
[[nodiscard]] std::string_view model_string(const t_type& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view();
    } else {
        return std::string_view(value);
    }
}

[[nodiscard]] inline bool is_email_like(std::string_view value) noexcept {
    const auto at = value.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= value.size() ||
        value.find('@', at + 1) != std::string_view::npos) {
        return false;
    }
    const auto domain = value.substr(at + 1);
    if (domain.front() == '.' || domain.back() == '.' ||
        domain.find('.') == std::string_view::npos ||
        domain.find("..") != std::string_view::npos) {
        return false;
    }
    for (const char c : value) {
        // Compare as unsigned: `char` is signed on most targets, so a UTF-8 byte
        // (>= 0x80) is negative and would satisfy `c <= 0x20`, wrongly rejecting an
        // internationalized address (RFC 6531) as if it held a control byte. The
        // guard only means to reject controls, SP, and DEL -- match the codebase's
        // other byte checks (e.g. is_valid_cookie_value) by using an unsigned byte.
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7F) {
            return false;
        }
    }
    return true;
}

template <typename t_type>
[[nodiscard]] bool is_empty_value(const t_type& value) noexcept {
    if constexpr (detail::is_ruvia_string<t_type> || detail::is_ruvia_bytes<t_type> ||
                  detail::is_ruvia_array<t_type> || detail::is_ruvia_boxed_array<t_type>) {
        return value.empty();
    } else {
        return false;
    }
}

template <typename t_type>
[[nodiscard]] constexpr std::string_view expected_type_name() noexcept {
    using value_type = std::remove_cvref_t<t_type>;
    if constexpr (detail::is_ruvia_bytes<value_type>) {
        return "must be a padded base64 string";
    } else if constexpr (detail::is_ruvia_string<value_type>) {
        return "must be a string";
    } else if constexpr (detail::is_ruvia_array<value_type> || detail::is_ruvia_boxed_array<value_type>) {
        return "must be an array";
    } else if constexpr (detail::is_ruvia_json_object<value_type> || is_model<value_type>) {
        return "must be an object";
    } else if constexpr (detail::is_ruvia_json_value<value_type>) {
        return "must be a JSON value";
    } else if constexpr (detail::is_ruvia_scalar<value_type>) {
        if constexpr (std::is_same_v<detail::model_scalar_value_type<value_type>, bool>) {
            return "must be a boolean";
        } else {
            return "must be a number";
        }
    } else {
        return "has invalid type";
    }
}

template <typename t_type>
[[nodiscard]] constexpr bool model_has_size_rule() noexcept {
    using value_type = std::remove_cvref_t<t_type>;
    return detail::is_ruvia_string<value_type> || detail::is_ruvia_bytes<value_type> ||
           detail::is_ruvia_array<value_type> || detail::is_ruvia_boxed_array<value_type>;
}

template <typename t_type>
[[nodiscard]] constexpr bool model_has_number_rule() noexcept {
    using value_type = std::remove_cvref_t<t_type>;
    if constexpr (detail::is_ruvia_scalar<value_type>) {
        using scalar_type = detail::model_scalar_value_type<value_type>;
        return std::is_arithmetic_v<scalar_type> && !std::is_same_v<scalar_type, bool>;
    } else {
        return false;
    }
}

// A validation path as segments on the traversal stack. Text is rendered only
// when an issue is emitted, so successful validation builds no path strings.
class model_path final {
public:
    constexpr model_path() noexcept = default;

    constexpr model_path(const model_path& parent, std::string_view field) noexcept
        : parent_(&parent),
          field_(field) {}

    constexpr model_path(const model_path& parent, std::size_t index) noexcept
        : parent_(&parent),
          index_(index),
          indexed_(true) {}

    model_path(const model_path&) = delete;
    model_path& operator=(const model_path&) = delete;

    [[nodiscard]] constexpr std::optional<std::string_view> single_field() const noexcept {
        if (parent_ != nullptr && parent_->parent_ == nullptr && !indexed_) {
            return field_;
        }
        return std::nullopt;
    }

    [[nodiscard]] constexpr std::size_t size_hint() const noexcept {
        if (parent_ == nullptr) {
            return 0;
        }
        const auto parent_size = parent_->size_hint();
        if (indexed_) {
            return parent_size + 2U + http_unsigned_decimal_size(index_);
        }
        return parent_size + (parent_size == 0 ? 0U : 1U) + field_.size();
    }

    [[nodiscard]] std::string_view render_to(std::span<char> buffer) const noexcept {
        std::size_t length = 0;
        append_to_buffer(buffer, length);
        return std::string_view(buffer.data(), length);
    }

    // Cold path for paths beyond the issue sink's stack buffer; size_hint() is exact.
    void render(std::string& output) const {
        output.resize(size_hint());
        output.resize(render_to(output).size());
    }

private:
    void append_to_buffer(std::span<char> buffer, std::size_t& length) const noexcept {
        if (parent_ == nullptr) {
            return;
        }
        parent_->append_to_buffer(buffer, length);
        if (indexed_) {
            buffer[length++] = '[';
            std::array<char, http_max_unsigned_decimal_size> digits;
            const auto formatted = http_format_unsigned(digits, index_);
            std::memcpy(buffer.data() + length, formatted.data(), formatted.size());
            length += formatted.size();
            buffer[length++] = ']';
            return;
        }
        if (length != 0) {
            buffer[length++] = '.';
        }
        if (!field_.empty()) {
            std::memcpy(buffer.data() + length, field_.data(), field_.size());
            length += field_.size();
        }
    }

    const model_path* parent_{nullptr};
    std::string_view field_{};
    std::size_t index_{0};
    bool indexed_{false};
};

// Forwards issues to a validator, rendering typical paths into a stack buffer
// and spilling only oversized paths to a reusable string.
template <typename validator_type>
class model_issue_sink final {
public:
    explicit model_issue_sink(validator_type& validator_value) noexcept
        : validator_(validator_value) {}

    void add(const model_path& path, std::string_view code, std::string_view message) {
        if (const auto single = path.single_field()) {
            validator_.add(*single, code, message);
            return;
        }
        static constexpr std::size_t inline_capacity = 128;
        if (path.size_hint() <= inline_capacity) {
            std::array<char, inline_capacity> buffer;
            validator_.add(path.render_to(buffer), code, message);
            return;
        }
        if (!path_) {
            path_.emplace();
        }
        path.render(*path_);
        validator_.add(*path_, code, message);
    }

private:
    validator_type& validator_;
    std::optional<std::string> path_;
};

template <typename rule_type>
[[nodiscard]] constexpr bool is_default_rule() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_default_rule_marker_type; };
}

template <auto provider>
struct initial final {
    using ruvia_initial_rule_marker_type = void;
    using ruvia_model_option_marker_type = void;

    [[nodiscard]] static constexpr decltype(auto) value() {
        return provider();
    }
};

template <typename rule_type>
[[nodiscard]] constexpr bool is_initial_rule() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_initial_rule_marker_type; };
}

template <typename rule_type>
[[nodiscard]] constexpr bool is_model_option() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_model_option_marker_type; };
}

template <typename rule_type>
[[nodiscard]] constexpr bool is_validation_rule() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_validation_rule_marker_type; };
}

template <auto bound_type>
[[nodiscard]] consteval bool valid_model_size_bound() {
    if constexpr (std::is_integral_v<decltype(bound_type)>) {
        return std::in_range<std::size_t>(bound_type);
    } else {
        return false;
    }
}

// Whether `value` (its size, for strings, bytes and arrays) lies beyond the
// bound in the direction `beyond` tests.
template <auto bound_type, typename value_type, typename compare_type>
[[nodiscard]] bool beyond_model_bound(const value_type& value, compare_type beyond) {
    if constexpr (model_has_size_rule<value_type>()) {
        static_assert(valid_model_size_bound<bound_type>(),
            "size rules require a nonnegative integral bound representable as size_t");
        return beyond(model_size(value), bound_type);
    } else if constexpr (model_has_number_rule<value_type>()) {
        using number_type = detail::model_scalar_value_type<value_type>;
        if constexpr (std::is_integral_v<number_type>) {
            static_assert(std::is_integral_v<decltype(bound_type)>,
                "integer fields require an integral bound");
        }
        return beyond(value, bound_type);
    } else {
        static_assert(detail::always_false<value_type>,
            "RUVIA_MIN/MAX require a numeric, string, bytes, or array field");
    }
}

template <typename value_type, typename validator_type, auto bound_type, fixed_string message>
void validate_rule(
    const value_type& value, const model_path& path, validator_type& validator_value, const min<bound_type, message>&) {
    if (beyond_model_bound<bound_type>(value, [](const auto& left, const auto& right) {
            return detail::validation_less(left, right);
        })) {
        validator_value.add(path, "too_small", message.view());
    }
}

template <typename value_type, typename validator_type, auto bound_type, fixed_string message>
void validate_rule(
    const value_type& value, const model_path& path, validator_type& validator_value, const max<bound_type, message>&) {
    if (beyond_model_bound<bound_type>(value, [](const auto& left, const auto& right) {
            return detail::validation_greater(left, right);
        })) {
        validator_value.add(path, "too_big", message.view());
    }
}

template <typename value_type, typename validator_type, fixed_string message, fixed_string... values>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value,
    const one_of<message, values...>&) {
    const auto actual = model_string(value);
    if (!((actual == values.view()) || ...)) {
        validator_value.add(path, "one_of", message.view());
    }
}

template <typename value_type, typename validator_type, fixed_string message>
void validate_rule(
    const value_type& value, const model_path& path, validator_type& validator_value, const email<message>&) {
    if (!is_email_like(model_string(value))) {
        validator_value.add(path, "email", message.view());
    }
}

template <typename value_type, typename validator_type, fixed_string pattern, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value,
    const pattern_rule<pattern, message>&) {
    const auto actual = model_string(value);
    if (!match_pattern_plan<pattern>(actual)) {
        validator_value.add(path, "pattern", message.view());
    }
}

template <typename value_type, typename validator_type, auto predicate, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value,
    const custom<predicate, message>&) {
    if (!predicate(value)) {
        validator_value.add(path, "custom", message.view());
    }
}

template <typename... rule_types>
class rules final {
public:
    constexpr rules() noexcept {
        static_assert((is_validation_rule<rule_types>() && ... && true),
            "field rules must be RUVIA_MIN, RUVIA_MAX, RUVIA_ONE_OF, RUVIA_EMAIL, RUVIA_PATTERN, "
            "or RUVIA_CUSTOM");
    }

    template <typename value_type, typename validator_type>
    void validate(model_field_state state_value, const std::optional<value_type>& value,
        [[maybe_unused]] const model_path& path, [[maybe_unused]] validator_type& validator_value) const {
        if (state_value != model_field_state::parsed || !value) {
            return;
        }
        (validate_rule(*value, path, validator_value, rule_types{}), ...);
    }
};

template <typename... descriptor_types>
struct model_schema final {};

struct empty_model_tag final {};

struct model_access final {
    template <typename model_type>
    [[nodiscard]] static constexpr auto schema() noexcept {
        return typename std::remove_cvref_t<model_type>::ruvia_model_schema_type{};
    }

    template <std::size_t index, typename model_type>
    [[nodiscard]] static decltype(auto) slot(model_type& value) noexcept {
        return value.fields_.template slot<index>();
    }

    template <typename model_type>
    [[nodiscard]] static model_type empty() {
        return model_type(empty_model_tag{});
    }
};

template <typename model_type, typename... descriptor_types, typename visitor_type, std::size_t... indices>
constexpr void visit_model_fields_impl(model_type& model, model_schema<descriptor_types...>, visitor_type&& visitor,
    std::index_sequence<indices...>) {
    auto& visitor_ref = visitor;
    (visitor_ref(descriptor_types{}, model_access::slot<indices>(model)), ...);
}

template <typename model_type, typename... descriptor_types, typename visitor_type>
constexpr void visit_model_fields(
    model_type& model, model_schema<descriptor_types...> schema, visitor_type&& visitor) {
    visit_model_fields_impl(
        model, schema, std::forward<visitor_type>(visitor), std::index_sequence_for<descriptor_types...>{});
}

}  // namespace ruvia::detail::model

namespace ruvia::detail {

struct model_validation_access final {
    template <typename model_type>
    [[nodiscard]] static bool structure_valid(const model_type& model) {
        bool valid = true;
        model::visit_model_fields(model, model::model_access::schema<model_type>(), [&](const auto&, const auto& slot) {
            using slot_type = std::remove_cvref_t<decltype(slot)>;
            const auto state_value = slot.state();
            if (state_value == model_field_state::duplicate || state_value == model_field_state::invalid_type ||
                (slot_type::required && state_value == model_field_state::missing)) {
                valid = false;
                return;
            }
            if constexpr (contains_model<typename slot_type::value_type>()) {
                if (const auto& value = slot.value(); value && !value_structure_valid(*value)) {
                    valid = false;
                }
            }
        });
        return valid;
    }

    template <typename model_type, typename validator_type>
    static void validate_model(const model_type& model_value, validator_type& validator_value) {
        model::model_issue_sink sink(validator_value);
        const model::model_path root;
        validate_structure(model_value, root, sink);
        validate_field_rules(model_value, root, sink);
    }

    template <typename value_type>
    [[nodiscard]] static consteval bool contains_model() noexcept {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            return true;
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            return contains_model<typename t_type::value_type>();
        } else {
            return false;
        }
    }

    template <typename value_type, typename validator_type>
    static void validate_value(const value_type& value, validator_type& validator_value) {
        model::model_issue_sink sink(validator_value);
        const model::model_path root;
        validate_value_structure(value, root, sink);
        validate_nested_field_rules(value, root, sink);
    }
    template <typename value_type>
    [[nodiscard]] static bool value_rules_valid(const value_type& value) {
        struct boolean_rule_sink final {
            bool valid_{true};
            void add(const model::model_path&, std::string_view, std::string_view) noexcept {
                valid_ = false;
            }
        } sink;
        const model::model_path root;
        validate_nested_field_rules(value, root, sink);
        return sink.valid_;
    }
    template <typename value_type>
    [[nodiscard]] static bool value_structure_valid(const value_type& value) {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            return structure_valid(value);
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_type = typename t_type::value_type;
            if constexpr (contains_model<element_type>()) {
                for (const auto& element : value) {
                    if (!value_structure_valid(element)) {
                        return false;
                    }
                }
            }
            return true;
        } else {
            return true;
        }
    }

private:
    template <typename model_type, typename sink_type>
    static void validate_structure(
        const model_type& model_value, const model::model_path& prefix, sink_type& sink) {
        model::visit_model_fields(
            model_value, model::model_access::schema<model_type>(), [&](const auto&, const auto& slot) {
                using slot_type = std::remove_cvref_t<decltype(slot)>;
                const model::model_path path(prefix, slot.wire_name());

                switch (slot.state()) {
                    case model_field_state::duplicate:
                        sink.add(path, "duplicate", "is duplicated");
                        return;
                    case model_field_state::invalid_type:
                        sink.add(path, "invalid_type", model::expected_type_name<typename slot_type::value_type>());
                        return;
                    case model_field_state::missing:
                        if constexpr (slot_type::required) {
                            sink.add(path, "required", "is required");
                        }
                        return;
                    case model_field_state::null:
                        return;
                    case model_field_state::parsed:
                        break;
                }

                if constexpr (contains_model<typename slot_type::value_type>()) {
                    validate_value_structure(*slot.value(), path, sink);
                }
            });
    }

    template <typename model_type, typename sink_type>
    static void validate_field_rules(
        const model_type& model_value, const model::model_path& prefix, sink_type& sink) {
        model::visit_model_fields(
            model_value, model::model_access::schema<model_type>(), [&](const auto& descriptor, const auto& slot) {
                using descriptor_type = std::remove_cvref_t<decltype(descriptor)>;
                using value_type = typename descriptor_type::value_type;
                if constexpr (descriptor_type::has_field_rules || contains_model<value_type>()) {
                    const model::model_path path(prefix, slot.wire_name());
                    if constexpr (descriptor_type::has_field_rules) {
                        typename descriptor_type::rules_type{}.validate(slot.state(), slot.value(), path, sink);
                    }
                    if constexpr (contains_model<value_type>()) {
                        if (slot.state() == model_field_state::parsed && slot.value()) {
                            validate_nested_field_rules(*slot.value(), path, sink);
                        }
                    }
                }
            });
    }

    template <typename value_type, typename sink_type>
    static void validate_value_structure(
        const value_type& value, const model::model_path& path, sink_type& sink) {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            validate_structure(value, path, sink);
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_type = typename t_type::value_type;
            if constexpr (contains_model<element_type>()) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    const model::model_path item_path(path, index++);
                    validate_value_structure(element, item_path, sink);
                }
            }
        }
    }

    template <typename value_type, typename sink_type>
    static void validate_nested_field_rules(
        const value_type& value, const model::model_path& path, sink_type& sink) {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            validate_field_rules(value, path, sink);
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_type = typename t_type::value_type;
            if constexpr (contains_model<element_type>()) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    const model::model_path item_path(path, index++);
                    validate_nested_field_rules(element, item_path, sink);
                }
            }
        }
    }
};

}  // namespace ruvia::detail
