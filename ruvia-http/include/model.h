#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "byte_view.h"
#include "decimal_text.h"
#include "fixed_string.h"
#include "http_header.h"
#include "json_codec.h"
#include "model_value.h"
#include "url_encoding.h"
#include "utf8.h"

namespace ruvia {

inline constexpr std::size_t max_validation_text_bytes = 1024;

struct validation_issue_options final {
    borrowed_text field_{};
    borrowed_text code_{};
    borrowed_text message_{};
};

class validation_issue final {
public:
    explicit validation_issue(validation_issue_options options) :
        field_(utf8_prefix(options.field_, max_validation_text_bytes)),
        code_(utf8_prefix(options.code_, max_validation_text_bytes)),
        message_(utf8_prefix(options.message_, max_validation_text_bytes)) {}

    validation_issue(const validation_issue&) = default;
    validation_issue(validation_issue&&) noexcept = default;

    validation_issue& operator=(const validation_issue& other) {
        if (this != &other) {
            validation_issue candidate(other);
            swap(candidate);
        }
        return *this;
    }

    validation_issue& operator=(validation_issue&& other) noexcept {
        if (this != &other) {
            swap(other);
            other.field_.clear();
            other.code_.clear();
            other.message_.clear();
        }
        return *this;
    }

    [[nodiscard]] std::string_view field() const& noexcept RUVIA_LIFETIMEBOUND {
        return field_;
    }
    std::string_view field() const&& = delete;

    [[nodiscard]] std::string_view code() const& noexcept RUVIA_LIFETIMEBOUND {
        return code_;
    }
    std::string_view code() const&& = delete;

    [[nodiscard]] std::string_view message() const& noexcept RUVIA_LIFETIMEBOUND {
        return message_;
    }
    std::string_view message() const&& = delete;

    [[nodiscard]] friend bool operator==(const validation_issue&, const validation_issue&) noexcept = default;

private:
    void swap(validation_issue& other) noexcept {
        field_.swap(other.field_);
        code_.swap(other.code_);
        message_.swap(other.message_);
    }

    std::string field_;
    std::string code_;
    std::string message_;
};

using borrowed_validation_issues = borrowed_span<validation_issue>;

struct model_parse_options final {
    std::size_t max_array_elements_{64 * 1024};
    std::size_t max_representation_bytes_{16 * 1024 * 1024};
    bool reject_unknown_fields_{false};
};

class model_parse_error final {
public:
    enum class code_type : std::uint8_t {
        invalid_format,
        validation_failed,
    };

    explicit model_parse_error(code_type code, std::string_view message, std::vector<validation_issue> issues = {}) :
        code_(code),
        message_(message),
        issues_(std::move(issues)) {}
    model_parse_error(const model_parse_error&) = delete;
    model_parse_error& operator=(const model_parse_error&) = delete;
    model_parse_error(model_parse_error&&) noexcept = default;
    model_parse_error& operator=(model_parse_error&&) = delete;

    [[nodiscard]] code_type code() const noexcept {
        return code_;
    }
    [[nodiscard]] std::string_view message() const& noexcept RUVIA_LIFETIMEBOUND {
        return message_;
    }
    std::string_view message() const&& = delete;
    [[nodiscard]] const std::vector<validation_issue>& issues() const& noexcept RUVIA_LIFETIMEBOUND {
        return issues_;
    }
    const std::vector<validation_issue>& issues() const&& = delete;
    [[nodiscard]] std::string take_message() && noexcept {
        return std::exchange(message_, {});
    }
    std::string take_message() & = delete;
    [[nodiscard]] std::vector<validation_issue> take_issues() && noexcept {
        auto issues = std::move(issues_);
        issues_.clear();
        return issues;
    }

private:
    code_type code_;
    std::string message_;
    std::vector<validation_issue> issues_;
};

template <typename t_type>
class model_parse_result final {
public:
    explicit model_parse_result(t_type&& value) noexcept(std::is_nothrow_move_constructible_v<t_type>) :
        state_(std::in_place_index<0>, std::move(value)) {}
    explicit model_parse_result(model_parse_error&& error) noexcept :
        state_(std::in_place_index<1>, std::move(error)) {}
    model_parse_result(const model_parse_result&) = delete;
    model_parse_result& operator=(const model_parse_result&) = delete;
    model_parse_result(model_parse_result&&) noexcept(std::is_nothrow_move_constructible_v<t_type>) = default;
    model_parse_result& operator=(model_parse_result&&) = delete;

    [[nodiscard]] bool has_value() const noexcept {
        return state_.index() == 0;
    }
    [[nodiscard]] const t_type& value() const& RUVIA_LIFETIMEBOUND {
        if (!has_value()) {
            throw std::logic_error("model parse result has no value");
        }
        return std::get<0>(state_);
    }
    const t_type& value() const&& = delete;
    [[nodiscard]] t_type take_value() && {
        if (!has_value()) {
            throw std::logic_error("model parse result has no value");
        }
        return std::move(std::get<0>(state_));
    }
    [[nodiscard]] const model_parse_error& error() const& RUVIA_LIFETIMEBOUND {
        if (has_value()) {
            throw std::logic_error("model parse result has no error");
        }
        return std::get<1>(state_);
    }
    const model_parse_error& error() const&& = delete;
    [[nodiscard]] model_parse_error take_error() && {
        if (has_value()) {
            throw std::logic_error("model parse result has no error");
        }
        return std::move(std::get<1>(state_));
    }

private:
    std::variant<t_type, model_parse_error> state_;
};

class json_object;
class json_value;

template <fixed_string tag_name, typename model_type>
struct tagged_case final {
    static constexpr auto tag = tag_name;
    using type = model_type;
};

template <fixed_string discriminator_field, typename... case_types>
class tagged_variant;

namespace detail {

enum class model_field_state : std::uint8_t {
    missing,
    parsed,
    null,
    invalid_type,
    duplicate,
    element_invalid,
};

template <typename t_type>
inline constexpr bool is_ruvia_json_value = std::is_same_v<std::remove_cvref_t<t_type>, json_value>;

template <typename t_type>
inline constexpr bool is_ruvia_json_object = std::is_same_v<std::remove_cvref_t<t_type>, json_object>;

template <typename t_type>
struct tagged_variant_traits : std::false_type {};

template <fixed_string discriminator, typename... case_types>
struct tagged_variant_traits<tagged_variant<discriminator, case_types...>> : std::true_type {};

template <typename t_type>
inline constexpr bool is_tagged_variant = tagged_variant_traits<std::remove_cvref_t<t_type>>::value;

template <typename t_type>
inline constexpr bool is_form_scalar_or_text = is_ruvia_string<t_type> || is_model_scalar<t_type> || is_ruvia_enum<t_type> || is_ruvia_uuid<t_type> || is_ruvia_decimal<t_type> || is_chrono_timestamp<t_type> || is_civil_date<t_type>;

template <typename t_type>
inline constexpr bool is_form_field = is_form_scalar_or_text<t_type> || (is_ruvia_array<t_type> && [] {
    if constexpr (is_ruvia_array<t_type>) {
        return is_form_scalar_or_text<typename std::remove_cvref_t<t_type>::value_type>;
    }
    return false;
}());

template <typename t_type>
inline constexpr bool is_model = requires { typename std::remove_cvref_t<t_type>::ruvia_model_schema_type; };

template <typename t_type>
inline constexpr bool model_field_value = is_ruvia_string<t_type> || is_ruvia_bytes<t_type> || is_model_scalar<t_type> || is_ruvia_enum<t_type> || is_ruvia_uuid<t_type> || is_ruvia_decimal<t_type> || is_chrono_timestamp<t_type> || is_civil_date<t_type> || is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type> || is_model<t_type> || is_tagged_variant<t_type>;

template <typename value_type>
inline constexpr bool model_field_value<array<value_type>> = model_field_value<std::remove_cvref_t<value_type>>;

template <typename value_type>
inline constexpr bool model_field_value<boxed_array<value_type>> = model_field_value<std::remove_cvref_t<value_type>>;

template <typename value_type>
inline constexpr bool model_field_value<map<value_type>> = model_field_value<std::remove_cvref_t<value_type>>;

template <typename t_type>
inline constexpr bool is_model_field = model_field_value<std::remove_cvref_t<t_type>>;

template <typename t_type>
inline constexpr bool is_model_json_value = is_model_field<t_type>;

template <typename t_type>
inline constexpr bool is_validation_number = (std::is_arithmetic_v<std::remove_cvref_t<t_type>> && !std::is_same_v<std::remove_cvref_t<t_type>, bool>) || is_ruvia_decimal<t_type>;

template <typename integer_type, typename floating_type>
[[nodiscard]] std::partial_ordering compare_validation_integer_floating(integer_type integer, floating_type floating) noexcept {
    if (std::isnan(floating)) {
        return std::partial_ordering::unordered;
    }
    const auto upper = std::ldexp(floating_type{1}, std::numeric_limits<integer_type>::digits);
    if (floating >= upper) {
        return std::partial_ordering::less;
    }
    const auto lower = std::is_signed_v<integer_type> ? -upper : floating_type{0};
    if (floating < lower) {
        return std::partial_ordering::greater;
    }
    const auto truncated = static_cast<integer_type>(floating);
    if (std::cmp_less(integer, truncated)) {
        return std::partial_ordering::less;
    }
    if (std::cmp_greater(integer, truncated)) {
        return std::partial_ordering::greater;
    }
    const auto whole = std::trunc(floating);
    if (whole < floating) {
        return std::partial_ordering::less;
    }
    if (whole > floating) {
        return std::partial_ordering::greater;
    }
    return std::partial_ordering::equivalent;
}

template <typename left_type, typename right_type>
[[nodiscard]] bool validation_less(const left_type& left, const right_type& right) {
    using l_t = std::remove_cvref_t<left_type>;
    using r_t = std::remove_cvref_t<right_type>;
    if constexpr (is_ruvia_decimal<l_t> && is_ruvia_decimal<r_t>) {
        return left < right;
    } else if constexpr (is_ruvia_decimal<l_t> && std::is_integral_v<r_t>) {
        return left < decimal(static_cast<std::int64_t>(right));
    } else if constexpr (std::is_integral_v<l_t> && is_ruvia_decimal<r_t>) {
        return decimal(static_cast<std::int64_t>(left)) < right;
    } else if constexpr (is_ruvia_decimal<l_t> && (std::same_as<r_t, std::string_view> || std::convertible_to<const r_t&, std::string_view>)) {
        return left < decimal(std::string_view(right));
    } else if constexpr (!is_validation_number<l_t> || !is_validation_number<r_t>) {
        return left < right;
    } else if constexpr (std::is_integral_v<l_t> && std::is_integral_v<r_t>) {
        return std::cmp_less(+left, +right);
    } else if constexpr (std::is_integral_v<l_t> && std::is_floating_point_v<r_t>) {
        return compare_validation_integer_floating(+left, right) == std::partial_ordering::less;
    } else if constexpr (std::is_floating_point_v<l_t> && std::is_integral_v<r_t>) {
        return compare_validation_integer_floating(+right, left) == std::partial_ordering::greater;
    } else {
        return left < right;
    }
}

template <typename left_type, typename right_type>
[[nodiscard]] bool validation_greater(const left_type& left, const right_type& right) {
    using l_t = std::remove_cvref_t<left_type>;
    using r_t = std::remove_cvref_t<right_type>;
    if constexpr (!is_ruvia_decimal<l_t> && !is_ruvia_decimal<r_t> && (!is_validation_number<l_t> || !is_validation_number<r_t>)) {
        if constexpr (requires { left > right; }) {
            return left > right;
        } else {
            return right < left;
        }
    } else {
        return validation_less(right, left);
    }
}

template <typename left_type, typename right_type>
[[nodiscard]] bool validation_less_equal(const left_type& left, const right_type& right) {
    return !validation_greater(left, right);
}

template <typename left_type, typename right_type>
[[nodiscard]] bool validation_greater_equal(const left_type& left, const right_type& right) {
    return !validation_less(left, right);
}

}  // namespace detail
}  // namespace ruvia

namespace ruvia::detail::model {

enum class pattern_atom_kind : unsigned char {
    literal,
    digit,
    word,
    space,
};

enum class pattern_quantifier : unsigned char {
    one,
    zero_or_one,
    zero_or_more,
    one_or_more,
};

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

[[nodiscard]] constexpr bool consume_pattern_class_atom(std::string_view pattern, std::size_t end, std::size_t& cursor, pattern_class_member& member) noexcept {
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

[[nodiscard]] constexpr bool consume_pattern_class_member(std::string_view pattern, std::size_t end, std::size_t& cursor, pattern_class_member& member) noexcept {
    if (!consume_pattern_class_atom(pattern, end, cursor, member)) {
        return false;
    }
    if (cursor + 1 < end && pattern[cursor] == '-') {
        ++cursor;
        pattern_class_member last;
        if (!consume_pattern_class_atom(pattern, end, cursor, last) || member.kind_ != pattern_atom_kind::literal || last.kind_ != pattern_atom_kind::literal || member.first_ > last.first_) {
            return false;
        }
        member.last_ = last.first_;
    }
    return true;
}

constexpr void insert_pattern_member(pattern_byte_set& bytes_set, const pattern_class_member& member) noexcept {
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
            bytes_set.insert(static_cast<unsigned char>(byte));
        }
    }
}

[[nodiscard]] constexpr bool compile_pattern_class(std::string_view pattern, std::size_t begin, std::size_t end, pattern_byte_set& bytes_set) noexcept {
    if (begin >= end) {
        return false;
    }
    for (std::size_t cursor = begin; cursor < end;) {
        pattern_class_member member;
        if (!consume_pattern_class_member(pattern, end, cursor, member)) {
            return false;
        }
        insert_pattern_member(bytes_set, member);
    }
    return true;
}

inline constexpr std::size_t max_pattern_bytes = 256;

template <std::size_t capacity>
[[nodiscard]] constexpr bool append_pattern_atom(std::string_view pattern, std::size_t end, std::size_t& cursor_value, pattern_plan<capacity>& plan) noexcept {
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
[[nodiscard]] constexpr pattern_plan<capacity> compile_pattern_plan(std::string_view pattern) noexcept {
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

        if (cursor_value < pattern_end && (pattern[cursor_value] == '*' || pattern[cursor_value] == '+' || pattern[cursor_value] == '?')) {
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
    static constexpr std::size_t capacity = pattern.view().size() < max_pattern_bytes ? pattern.view().size() : max_pattern_bytes;
    static constexpr auto value = compile_pattern_plan<capacity>(pattern.view());
    static_assert(value.valid_,
        "RUVIA_PATTERN supports only anchored patterns using literals, '.', character classes, \\d, \\w, \\s, and '*', '+', '?'. "
        "Escaped letters and digits other than \\d, \\w, \\s (such as \\n or \\D) are unsupported. "
        "Ranges require ordered single-character endpoints. Use RUVIA_CUSTOM for other matching logic.");
};

inline constexpr std::size_t max_pattern_match_steps = 1'000'000;

template <std::size_t capacity>
[[nodiscard]] constexpr bool match_pattern_plan_from(const pattern_plan<capacity>& plan, std::string_view value, std::size_t atom_index, std::size_t value_index, std::size_t& budget) noexcept {
    if (budget == 0) {
        return false;
    }
    --budget;
    if (atom_index == plan.count_) {
        return value_index == value.size();
    }

    const auto& atom = plan.atoms_[atom_index];
    const std::size_t min_count = atom.quantifier_ == pattern_quantifier::one || atom.quantifier_ == pattern_quantifier::one_or_more ? 1 : 0;
    const std::size_t scan_limit = atom.quantifier_ == pattern_quantifier::one || atom.quantifier_ == pattern_quantifier::zero_or_one ? std::size_t{1} : value.size();
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

// Validation rule descriptors.
template <auto bound, fixed_string error_message>
struct min final {
    using ruvia_validation_rule_marker_type = void;
};

template <auto bound, fixed_string error_message>
struct max final {
    using ruvia_validation_rule_marker_type = void;
};

template <auto bound, fixed_string error_message>
struct gt final {
    using ruvia_validation_rule_marker_type = void;
};

template <auto bound, fixed_string error_message>
struct lt final {
    using ruvia_validation_rule_marker_type = void;
};

template <std::size_t bound, fixed_string error_message>
struct min_bytes final {
    using ruvia_validation_rule_marker_type = void;
};

template <std::size_t bound, fixed_string error_message>
struct max_bytes final {
    using ruvia_validation_rule_marker_type = void;
};

template <std::size_t bound, fixed_string error_message>
struct min_chars final {
    using ruvia_validation_rule_marker_type = void;
};

template <std::size_t bound, fixed_string error_message>
struct max_chars final {
    using ruvia_validation_rule_marker_type = void;
};

template <fixed_string error_message>
struct not_empty final {
    using ruvia_validation_rule_marker_type = void;
};

template <fixed_string error_message>
struct uuid_rule final {
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

template <fixed_string pattern, fixed_string error_message>
struct pattern_rule final {
    using ruvia_validation_rule_marker_type = void;
    static_assert(compiled_pattern_plan<pattern>::value.valid_, "RUVIA_PATTERN requires a valid pattern");
};

template <typename... inner_rule_types>
struct each_rule final {
    using ruvia_validation_rule_marker_type = void;
};

// Field options.
template <auto provider>
struct default_value final {
    using ruvia_default_rule_marker_type = void;
    using ruvia_model_option_marker_type = void;

    [[nodiscard]] static constexpr decltype(auto) value() {
        return provider();
    }
};

template <auto provider>
struct initial final {
    using ruvia_initial_rule_marker_type = void;
    using ruvia_model_option_marker_type = void;

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

struct stringify_int64 final {
    using ruvia_model_option_marker_type = void;
};

struct decimal_as_number final {
    using ruvia_model_option_marker_type = void;
};

template <bytes_encoding encoding_value>
struct bytes_codec_option final {
    using ruvia_bytes_codec_marker_type = void;
    using ruvia_model_option_marker_type = void;
    static constexpr bytes_encoding encoding = encoding_value;
};

template <fixed_string... alias_names>
struct field_aliases final {
    static_assert(sizeof...(alias_names) > 0, "RUVIA_ALIAS requires at least one wire alias");
    using ruvia_alias_option_marker_type = void;
    using ruvia_model_option_marker_type = void;
    static constexpr std::array<std::string_view, sizeof...(alias_names)> names{alias_names.view()...};
};

// Model-level options inside RUVIA_MODEL(...).
template <naming_convention convention_value>
struct model_naming_option final {
    using ruvia_model_level_naming_marker_type = void;
    using ruvia_model_level_option_marker_type = void;
    static constexpr naming_convention convention = convention_value;
};

template <auto check_fn>
struct model_validate_option final {
    using ruvia_model_level_validate_marker_type = void;
    using ruvia_model_level_option_marker_type = void;

    template <typename model_type, typename sink_type>
    static void invoke(const model_type& model_value, sink_type& sink) {
        check_fn(model_value, sink);
    }
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
    if (at == std::string_view::npos || at == 0 || at + 1 >= value.size() || value.find('@', at + 1) != std::string_view::npos) {
        return false;
    }
    const auto domain = value.substr(at + 1);
    if (domain.front() == '.' || domain.back() == '.' || domain.find('.') == std::string_view::npos || domain.find("..") != std::string_view::npos) {
        return false;
    }
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7F) {
            return false;
        }
    }
    return true;
}

template <typename t_type>
[[nodiscard]] bool is_empty_value(const t_type& value) noexcept {
    if constexpr (detail::is_ruvia_string<t_type> || detail::is_ruvia_bytes<t_type> || detail::is_ruvia_array<t_type> || detail::is_ruvia_boxed_array<t_type> || detail::is_ruvia_map<t_type>) {
        return value.empty();
    } else {
        return false;
    }
}

template <typename t_type, bytes_encoding bin_encoding = bytes_encoding::base64>
[[nodiscard]] constexpr std::string_view expected_type_name() noexcept {
    using value_type = std::remove_cvref_t<t_type>;
    if constexpr (detail::is_ruvia_bytes<value_type>) {
        if constexpr (bin_encoding == bytes_encoding::base64url) {
            return "must be a base64url string";
        } else if constexpr (bin_encoding == bytes_encoding::hex) {
            return "must be a hex string";
        } else {
            return "must be a padded base64 string";
        }
    } else if constexpr (detail::is_ruvia_string<value_type>) {
        return "must be a string";
    } else if constexpr (detail::is_ruvia_uuid<value_type>) {
        return "must be a UUID string";
    } else if constexpr (detail::is_ruvia_decimal<value_type>) {
        return "must be a decimal";
    } else if constexpr (detail::is_chrono_timestamp<value_type>) {
        return "must be an RFC 3339 timestamp";
    } else if constexpr (detail::is_civil_date<value_type>) {
        return "must be an ISO 8601 date";
    } else if constexpr (detail::is_ruvia_enum<value_type>) {
        return "must be a valid enum value";
    } else if constexpr (detail::is_ruvia_array<value_type> || detail::is_ruvia_boxed_array<value_type>) {
        return "must be an array";
    } else if constexpr (detail::is_ruvia_map<value_type> || detail::is_ruvia_json_object<value_type> || detail::is_model<value_type> || detail::is_tagged_variant<value_type>) {
        return "must be an object";
    } else if constexpr (detail::is_ruvia_json_value<value_type>) {
        return "must be a JSON value";
    } else if constexpr (std::is_same_v<value_type, bool>) {
        return "must be a boolean";
    } else if constexpr (detail::is_model_scalar<value_type>) {
        return "must be a number";
    } else {
        return "has invalid type";
    }
}

// D4: RUVIA_MIN / RUVIA_MAX apply to numbers, bytes, arrays, and maps.
// Strings use explicit RUVIA_MIN_BYTES / RUVIA_MAX_BYTES or RUVIA_MIN_CHARS / RUVIA_MAX_CHARS.
template <typename t_type>
[[nodiscard]] constexpr bool model_has_container_size_rule() noexcept {
    using value_type = std::remove_cvref_t<t_type>;
    return detail::is_ruvia_bytes<value_type> || detail::is_ruvia_array<value_type> || detail::is_ruvia_boxed_array<value_type> || detail::is_ruvia_map<value_type>;
}

template <typename t_type>
[[nodiscard]] constexpr bool model_has_number_rule() noexcept {
    using value_type = std::remove_cvref_t<t_type>;
    return (detail::is_model_scalar<value_type> && !std::is_same_v<value_type, bool>) || detail::is_ruvia_decimal<value_type>;
}

class model_path final {
public:
    constexpr model_path() noexcept = default;

    constexpr model_path(const model_path& parent, std::string_view field) noexcept :
        parent_(&parent),
        field_(field) {}

    constexpr model_path(const model_path& parent, std::size_t index) noexcept :
        parent_(&parent),
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
            return parent_size + 2U + decimal_size(index_);
        }
        if (field_.empty()) {
            return parent_size;
        }
        return parent_size + (parent_size == 0 ? 0U : 1U) + field_.size();
    }

    [[nodiscard]] std::string_view render_to(std::span<char> buffer RUVIA_LIFETIMEBOUND) const noexcept {
        std::size_t length = 0;
        append_to_buffer(buffer, length);
        return std::string_view(buffer.data(), length);
    }

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
            std::array<char, max_decimal_size> digits;
            const auto formatted = format_decimal(digits, index_);
            std::memcpy(buffer.data() + length, formatted.data(), formatted.size());
            length += formatted.size();
            buffer[length++] = ']';
            return;
        }
        if (field_.empty()) {
            return;
        }
        if (length != 0) {
            buffer[length++] = '.';
        }
        std::memcpy(buffer.data() + length, field_.data(), field_.size());
        length += field_.size();
    }

    const model_path* parent_{nullptr};
    std::string_view field_{};
    std::size_t index_{0};
    bool indexed_{false};
};

template <typename validator_type>
class model_issue_sink final {
public:
    explicit model_issue_sink(validator_type& validator_value) noexcept :
        validator_(validator_value) {}

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

// Scoped sink passed to RUVIA_MODEL_VALIDATE lambdas: `sink.add("end", "range", "...")`
// automatically prefixes the enclosing model's path.
template <typename sink_type>
class scoped_model_issue_sink final {
public:
    scoped_model_issue_sink(const model_path& prefix, sink_type& sink) noexcept :
        prefix_(prefix),
        sink_(sink) {}

    void add(std::string_view relative_field, std::string_view code, std::string_view message) {
        const model_path target(prefix_, relative_field);
        sink_.add(target, code, message);
    }

private:
    const model_path& prefix_;
    sink_type& sink_;
};

template <typename rule_type>
[[nodiscard]] constexpr bool is_default_rule() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_default_rule_marker_type; };
}

template <typename rule_type>
[[nodiscard]] constexpr bool is_initial_rule() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_initial_rule_marker_type; };
}

template <typename rule_type>
[[nodiscard]] constexpr bool is_bytes_codec_option() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_bytes_codec_marker_type; };
}

template <typename rule_type>
[[nodiscard]] constexpr bool is_alias_option() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_alias_option_marker_type; };
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

template <typename item_type>
[[nodiscard]] constexpr bool is_model_level_option() noexcept {
    using unwrapped_type = std::remove_cvref_t<item_type>;
    return requires { typename unwrapped_type::ruvia_model_level_option_marker_type; };
}

template <typename item_type>
[[nodiscard]] constexpr bool is_model_level_naming() noexcept {
    using unwrapped_type = std::remove_cvref_t<item_type>;
    return requires { typename unwrapped_type::ruvia_model_level_naming_marker_type; };
}

template <typename item_type>
[[nodiscard]] constexpr bool is_model_level_validate() noexcept {
    using unwrapped_type = std::remove_cvref_t<item_type>;
    return requires { typename unwrapped_type::ruvia_model_level_validate_marker_type; };
}

template <auto bound_type>
[[nodiscard]] consteval bool valid_model_size_bound() {
    if constexpr (std::is_integral_v<decltype(bound_type)> && !std::is_same_v<decltype(bound_type), bool>) {
        return std::in_range<std::size_t>(bound_type);
    } else {
        return false;
    }
}

template <auto bound_type, typename value_type, typename compare_type>
[[nodiscard]] bool beyond_model_bound(const value_type& value, compare_type beyond) {
    static_assert(!detail::is_ruvia_string<value_type>, "RUVIA_MIN/RUVIA_MAX do not accept ruvia::string; use RUVIA_MIN_BYTES/RUVIA_MAX_BYTES or RUVIA_MIN_CHARS/RUVIA_MAX_CHARS");
    if constexpr (model_has_container_size_rule<value_type>()) {
        static_assert(valid_model_size_bound<bound_type>(), "container size rules require a nonnegative integral bound representable as size_t");
        return beyond(model_size(value), bound_type);
    } else if constexpr (model_has_number_rule<value_type>()) {
        if constexpr (std::is_integral_v<std::remove_cvref_t<value_type>>) {
            static_assert(std::is_integral_v<decltype(bound_type)> && !std::is_same_v<decltype(bound_type), bool>, "integer fields require an integral bound");
        }
        if constexpr (detail::is_ruvia_decimal<value_type> && requires { bound_type.view(); }) {
            return beyond(value, decimal(bound_type.view()));
        } else {
            return beyond(value, bound_type);
        }
    } else {
        static_assert(detail::always_false<value_type>, "RUVIA_MIN/RUVIA_MAX require a numeric, bytes, array, or map field");
    }
}

template <auto bound_type, typename value_type, typename compare_type>
[[nodiscard]] bool beyond_strict_number_bound(const value_type& value, compare_type beyond) {
    static_assert(model_has_number_rule<value_type>(), "RUVIA_GT/RUVIA_LT require a numeric field");
    if constexpr (std::is_integral_v<std::remove_cvref_t<value_type>>) {
        static_assert(std::is_integral_v<decltype(bound_type)> && !std::is_same_v<decltype(bound_type), bool>, "integer fields require an integral bound");
    }
    if constexpr (detail::is_ruvia_decimal<value_type> && requires { bound_type.view(); }) {
        return beyond(value, decimal(bound_type.view()));
    } else {
        return beyond(value, bound_type);
    }
}

template <typename value_type, typename validator_type, auto bound_type, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const min<bound_type, message>&) {
    if (beyond_model_bound<bound_type>(value, [](const auto& left, const auto& right) {
        return detail::validation_less(left, right);
    })) {
        validator_value.add(path, "too_small", message.view());
    }
}

template <typename value_type, typename validator_type, auto bound_type, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const max<bound_type, message>&) {
    if (beyond_model_bound<bound_type>(value, [](const auto& left, const auto& right) {
        return detail::validation_greater(left, right);
    })) {
        validator_value.add(path, "too_big", message.view());
    }
}

template <typename value_type, typename validator_type, auto bound_type, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const gt<bound_type, message>&) {
    if (beyond_strict_number_bound<bound_type>(value, [](const auto& left, const auto& right) {
        return detail::validation_less_equal(left, right);
    })) {
        validator_value.add(path, "too_small", message.view());
    }
}

template <typename value_type, typename validator_type, auto bound_type, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const lt<bound_type, message>&) {
    if (beyond_strict_number_bound<bound_type>(value, [](const auto& left, const auto& right) {
        return detail::validation_greater_equal(left, right);
    })) {
        validator_value.add(path, "too_big", message.view());
    }
}

template <typename value_type, typename validator_type, std::size_t bound_value, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const min_bytes<bound_value, message>&) {
    static_assert(detail::is_ruvia_string<value_type> || detail::is_ruvia_bytes<value_type>, "RUVIA_MIN_BYTES requires a ruvia::string or ruvia::bytes field");
    if (value.size() < bound_value) {
        validator_value.add(path, "too_small", message.view());
    }
}

template <typename value_type, typename validator_type, std::size_t bound_value, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const max_bytes<bound_value, message>&) {
    static_assert(detail::is_ruvia_string<value_type> || detail::is_ruvia_bytes<value_type>, "RUVIA_MAX_BYTES requires a ruvia::string or ruvia::bytes field");
    if (value.size() > bound_value) {
        validator_value.add(path, "too_big", message.view());
    }
}

template <typename value_type, typename validator_type, std::size_t bound_value, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const min_chars<bound_value, message>&) {
    static_assert(detail::is_ruvia_string<value_type>, "RUVIA_MIN_CHARS requires a ruvia::string field");
    if (utf8_codepoint_count(value.view()) < bound_value) {
        validator_value.add(path, "too_small", message.view());
    }
}

template <typename value_type, typename validator_type, std::size_t bound_value, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const max_chars<bound_value, message>&) {
    static_assert(detail::is_ruvia_string<value_type>, "RUVIA_MAX_CHARS requires a ruvia::string field");
    if (utf8_codepoint_count(value.view()) > bound_value) {
        validator_value.add(path, "too_big", message.view());
    }
}

template <typename value_type, typename validator_type, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const not_empty<message>&) {
    static_assert(detail::is_ruvia_string<value_type> || detail::is_ruvia_bytes<value_type> || detail::is_ruvia_array<value_type> || detail::is_ruvia_boxed_array<value_type> || detail::is_ruvia_map<value_type>, "RUVIA_NOT_EMPTY requires a string, bytes, array, or map field");
    if (value.empty()) {
        validator_value.add(path, "empty", message.view());
    }
}

template <typename value_type, typename validator_type, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const uuid_rule<message>&) {
    static_assert(detail::is_ruvia_string<value_type>, "RUVIA_UUID requires a ruvia::string field (or use ruvia::uuid as the field type)");
    if (!uuid::try_parse(value.view()).has_value()) {
        validator_value.add(path, "uuid", message.view());
    }
}

template <typename value_type, typename validator_type, fixed_string message, fixed_string... values>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const one_of<message, values...>&) {
    const auto actual = model_string(value);
    if (!((actual == values.view()) || ...)) {
        validator_value.add(path, "one_of", message.view());
    }
}

template <typename value_type, typename validator_type, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const email<message>&) {
    if (!is_email_like(model_string(value))) {
        validator_value.add(path, "email", message.view());
    }
}

template <typename value_type, typename validator_type, fixed_string pattern, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const pattern_rule<pattern, message>&) {
    const auto actual = model_string(value);
    if (!match_pattern_plan<pattern>(actual)) {
        validator_value.add(path, "pattern", message.view());
    }
}

template <typename value_type, typename validator_type, auto predicate, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const custom<predicate, message>&) {
    if (!predicate(value)) {
        validator_value.add(path, "custom", message.view());
    }
}

template <typename value_type, typename validator_type, typename... inner_rule_types>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value, const each_rule<inner_rule_types...>&) {
    static_assert((is_validation_rule<inner_rule_types>() && ... && true), "RUVIA_EACH arguments must be validation rules");
    if constexpr (detail::is_ruvia_array<value_type> || detail::is_ruvia_boxed_array<value_type>) {
        std::size_t index = 0;
        for (const auto& element : value) {
            const model_path item_path(path, index++);
            (validate_rule(element, item_path, validator_value, inner_rule_types{}), ...);
        }
    } else if constexpr (detail::is_ruvia_map<value_type>) {
        for (const auto& entry : value) {
            const model_path entry_path(path, entry.key_.view());
            (validate_rule(entry.value_, entry_path, validator_value, inner_rule_types{}), ...);
        }
    } else {
        static_assert(detail::always_false<value_type>, "RUVIA_EACH requires an array, boxed_array, or map field");
    }
}

template <typename... rule_types>
class rules final {
public:
    constexpr rules() noexcept {
        static_assert((is_validation_rule<rule_types>() && ... && true), "invalid field validation rule");
    }

    template <typename value_type, typename validator_type>
    void validate(model_field_state state_value, const std::optional<value_type>& value, [[maybe_unused]] const model_path& path, [[maybe_unused]] validator_type& validator_value) const {
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

    template <typename model_type>
    [[nodiscard]] static constexpr auto model_validators() noexcept {
        return typename std::remove_cvref_t<model_type>::ruvia_model_validators_type{};
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
constexpr void visit_model_fields_impl(model_type& model, model_schema<descriptor_types...>, visitor_type&& visitor, std::index_sequence<indices...>) {
    auto& visitor_ref = visitor;
    (visitor_ref(descriptor_types{}, model_access::slot<indices>(model)), ...);
}

template <typename model_type, typename... descriptor_types, typename visitor_type>
constexpr void visit_model_fields(model_type& model, model_schema<descriptor_types...> schema, visitor_type&& visitor) {
    visit_model_fields_impl(model, schema, std::forward<visitor_type>(visitor), std::index_sequence_for<descriptor_types...>{});
}

template <typename model_type, typename sink_type, typename... validator_option_types>
void run_model_level_validators(const model_type& model_value, const model_path& prefix, sink_type& sink, std::tuple<validator_option_types...>) {
    if constexpr (sizeof...(validator_option_types) > 0) {
        scoped_model_issue_sink<sink_type> scoped(prefix, sink);
        (validator_option_types::invoke(model_value, scoped), ...);
    }
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
            if (state_value == model_field_state::duplicate || state_value == model_field_state::invalid_type || state_value == model_field_state::element_invalid || (slot_type::required && state_value == model_field_state::missing)) {
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
        if constexpr (is_model<t_type> || is_tagged_variant<t_type>) {
            return true;
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type> || is_ruvia_map<t_type>) {
            return contains_model<typename ruvia_container_element_type<t_type>::type>();
        } else {
            return false;
        }
    }

    template <typename t_type>
    struct ruvia_container_element_type {
        using type = typename t_type::value_type;
    };
    template <typename v_type>
    struct ruvia_container_element_type<map<v_type>> {
        using type = v_type;
    };

    template <typename value_type, typename validator_type>
    [[nodiscard]] static bool validate_value(const value_type& value, validator_type& validator_value) {
        model::model_issue_sink sink(validator_value);
        const model::model_path root;
        validate_value_structure(value, root, sink);
        const bool struct_ok = value_structure_valid(value);
        validate_nested_field_rules(value, root, sink);
        return struct_ok;
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
        } else if constexpr (is_tagged_variant<t_type>) {
            return value.visit([](const auto& alt) {
                return structure_valid(alt);
            });
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
        } else if constexpr (is_ruvia_map<t_type>) {
            using element_type = typename t_type::mapped_type;
            if constexpr (contains_model<element_type>()) {
                for (const auto& entry : value) {
                    if (!value_structure_valid(entry.value_)) {
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
    static void validate_structure(const model_type& model_value, const model::model_path& prefix, sink_type& sink) {
        model::visit_model_fields(model_value, model::model_access::schema<model_type>(), [&](const auto&, const auto& slot) {
            using slot_type = std::remove_cvref_t<decltype(slot)>;
            const model::model_path path(prefix, slot.wire_name());

            switch (slot.state()) {
                case model_field_state::duplicate:
                    sink.add(path, "duplicate", "is duplicated");
                    return;
                case model_field_state::invalid_type:
                    sink.add(path, "invalid_type", model::expected_type_name<typename slot_type::value_type, slot_type::binary_encoding>());
                    return;
                case model_field_state::element_invalid:
                    // Element-level issues were already recorded with exact [index] / .key paths.
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
    static void validate_field_rules(const model_type& model_value, const model::model_path& prefix, sink_type& sink) {
        model::visit_model_fields(model_value, model::model_access::schema<model_type>(), [&](const auto& descriptor, const auto& slot) {
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
        if (structure_valid(model_value)) {
            model::run_model_level_validators(model_value, prefix, sink, model::model_access::model_validators<model_type>());
        }
    }

    template <typename value_type, typename sink_type>
    static void validate_value_structure(const value_type& value, const model::model_path& path, sink_type& sink) {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            validate_structure(value, path, sink);
        } else if constexpr (is_tagged_variant<t_type>) {
            value.visit([&](const auto& alt) {
                validate_structure(alt, path, sink);
            });
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_type = typename t_type::value_type;
            if constexpr (contains_model<element_type>()) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    const model::model_path item_path(path, index++);
                    validate_value_structure(element, item_path, sink);
                }
            }
        } else if constexpr (is_ruvia_map<t_type>) {
            using element_type = typename t_type::mapped_type;
            if constexpr (contains_model<element_type>()) {
                for (const auto& entry : value) {
                    const model::model_path entry_path(path, entry.key_.view());
                    validate_value_structure(entry.value_, entry_path, sink);
                }
            }
        }
    }

    template <typename value_type, typename sink_type>
    static void validate_nested_field_rules(const value_type& value, const model::model_path& path, sink_type& sink) {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            validate_field_rules(value, path, sink);
        } else if constexpr (is_tagged_variant<t_type>) {
            value.visit([&](const auto& alt) {
                validate_field_rules(alt, path, sink);
            });
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_type = typename t_type::value_type;
            if constexpr (contains_model<element_type>()) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    const model::model_path item_path(path, index++);
                    validate_nested_field_rules(element, item_path, sink);
                }
            }
        } else if constexpr (is_ruvia_map<t_type>) {
            using element_type = typename t_type::mapped_type;
            if constexpr (contains_model<element_type>()) {
                for (const auto& entry : value) {
                    const model::model_path entry_path(path, entry.key_.view());
                    validate_nested_field_rules(entry.value_, entry_path, sink);
                }
            }
        }
    }
};

}  // namespace ruvia::detail

namespace ruvia::detail::model {

template <typename... option_types>
class model_options final {
    static_assert((is_model_option<option_types>() && ... && true), "invalid model field option");
    static_assert(((is_default_rule<option_types>() ? 1 : 0) + ... + 0) <= 1, "a model field may have at most one RUVIA_DEFAULT");
    static_assert(((is_initial_rule<option_types>() ? 1 : 0) + ... + 0) <= 1, "a model field may have at most one RUVIA_INITIAL");
    static_assert(((is_bytes_codec_option<option_types>() ? 1 : 0) + ... + 0) <= 1, "a model field may have at most one RUVIA_BYTES_* codec");
    static_assert(((is_alias_option<option_types>() ? 1 : 0) + ... + 0) <= 1, "a model field may have at most one RUVIA_ALIAS");

    template <typename option_type>
    [[nodiscard]] static consteval bytes_encoding extract_bytes_encoding() noexcept {
        if constexpr (is_bytes_codec_option<option_type>()) {
            return option_type::encoding;
        }
        return bytes_encoding::base64;
    }

    [[nodiscard]] static consteval bytes_encoding resolve_bytes_encoding() noexcept {
        bytes_encoding resolved = bytes_encoding::base64;
        ((is_bytes_codec_option<option_types>() ? (resolved = extract_bytes_encoding<option_types>(), 0) : 0), ...);
        return resolved;
    }

public:
    static constexpr bool nullable = (std::is_same_v<option_types, ::ruvia::detail::model::nullable> || ... || false);
    static constexpr bool has_initial = (is_initial_rule<option_types>() || ... || false);
    static constexpr bool has_default = (is_default_rule<option_types>() || ... || false);
    static constexpr bool has_bytes_codec = (is_bytes_codec_option<option_types>() || ... || false);
    static constexpr bytes_encoding binary_encoding = resolve_bytes_encoding();
    static constexpr bool stringify_int64 = (std::is_same_v<option_types, ::ruvia::detail::model::stringify_int64> || ... || false);
    static constexpr bool decimal_number = (std::is_same_v<option_types, ::ruvia::detail::model::decimal_as_number> || ... || false);

    [[nodiscard]] static constexpr bool emit_null() noexcept {
        return contains_option<::ruvia::detail::model::emit_null>();
    }

    [[nodiscard]] static constexpr bool omit_empty() noexcept {
        return contains_option<::ruvia::detail::model::omit_empty>();
    }

    [[nodiscard]] static constexpr bool matches_alias(std::string_view key, bool case_insensitive = false) noexcept {
        return (match_one_alias<option_types>(key, case_insensitive) || ... || false);
    }

    template <typename visitor_type>
    static constexpr void for_each_alias(visitor_type&& visitor) {
        (visit_one_alias<option_types>(visitor), ...);
    }

    template <typename consumer_type>
    static void apply_default(consumer_type&& consume) {
        if constexpr (sizeof...(option_types) != 0) {
            (apply_default_option<option_types>(consume), ...);
        } else {
            (void)consume;
        }
    }

    template <typename consumer_type>
    static void apply_initial(consumer_type&& consume) {
        if constexpr (sizeof...(option_types) != 0) {
            (apply_initial_option<option_types>(consume), ...);
        } else {
            (void)consume;
        }
    }

private:
    template <typename option_type>
    [[nodiscard]] static constexpr bool contains_option() noexcept {
        return (std::is_same_v<std::remove_cvref_t<option_types>, option_type> || ... || false);
    }

    template <typename option_type>
    [[nodiscard]] static constexpr bool match_one_alias(std::string_view key, bool case_insensitive) noexcept {
        if constexpr (is_alias_option<option_type>()) {
            for (const auto alias : option_type::names) {
                if (case_insensitive ? http_ascii_equals_ignore_case(key, alias) : (key == alias)) {
                    return true;
                }
            }
        }
        return false;
    }

    template <typename option_type, typename visitor_type>
    static constexpr void visit_one_alias(visitor_type& visitor) {
        if constexpr (is_alias_option<option_type>()) {
            for (const auto alias : option_type::names) {
                visitor(alias);
            }
        }
    }

    template <typename option_type, typename consumer_type>
    static void apply_initial_option(consumer_type& consume) {
        if constexpr (is_initial_rule<option_type>()) {
            consume(option_type::value());
        } else {
            (void)consume;
        }
    }

    template <typename option_type, typename consumer_type>
    static void apply_default_option(consumer_type& consume) {
        if constexpr (is_default_rule<option_type>()) {
            consume(option_type::value());
        } else {
            (void)consume;
        }
    }
};

template <typename field_type, typename value_type>
void assign_field_value(std::optional<field_type>& target, value_type&& value) {
    using raw_input_type = std::remove_cvref_t<value_type>;
    if constexpr (std::is_same_v<field_type, bool>) {
        static_assert(std::is_same_v<raw_input_type, bool>, "bool model fields require an exact bool value");
        target.emplace(value);
    } else if constexpr (detail::is_model_integer<field_type>) {
        static_assert(detail::is_model_integer<raw_input_type>, "integer model fields require an integer value");
        const auto checked = detail::checked_model_integer<field_type>(value);
        target.emplace(checked);
    } else if constexpr (detail::is_model_floating<field_type>) {
        if constexpr (std::is_floating_point_v<raw_input_type>) {
            const auto checked = detail::checked_model_float<field_type>(value);
            target.emplace(checked);
        } else if constexpr (detail::is_model_integer<raw_input_type>) {
            target.emplace(static_cast<field_type>(value));
        } else {
            static_assert(detail::always_false<raw_input_type>, "floating-point model fields require an arithmetic value");
        }
    } else if constexpr (detail::is_ruvia_string<field_type> && std::is_same_v<raw_input_type, std::string> && std::is_rvalue_reference_v<value_type&&>) {
        field_type field;
        field.assign(std::forward<value_type>(value));
        target.emplace(std::move(field));
    } else if constexpr (detail::is_ruvia_string<field_type> && std::is_convertible_v<value_type&&, std::string_view> && !std::is_same_v<raw_input_type, field_type>) {
        field_type field;
        field.assign(std::string_view(std::forward<value_type>(value)));
        target.emplace(std::move(field));
    } else if constexpr (detail::is_ruvia_bytes<field_type> && !std::is_same_v<raw_input_type, field_type>) {
        field_type field;
        if constexpr (std::is_same_v<raw_input_type, std::vector<std::byte>> && std::is_rvalue_reference_v<value_type&&>) {
            field.assign(std::forward<value_type>(value));
        } else {
            field.assign(byte_view(std::forward<value_type>(value)));
        }
        target.emplace(std::move(field));
    } else if constexpr (detail::is_ruvia_uuid<field_type> && std::is_convertible_v<value_type&&, std::string_view>) {
        target.emplace(uuid(std::string_view(std::forward<value_type>(value))));
    } else if constexpr (detail::is_ruvia_decimal<field_type> && std::is_convertible_v<value_type&&, std::string_view>) {
        target.emplace(decimal(std::string_view(std::forward<value_type>(value))));
    } else if constexpr (detail::is_ruvia_decimal<field_type> && detail::is_model_integer<raw_input_type>) {
        target.emplace(decimal(detail::checked_model_integer<std::int64_t>(value)));
    } else {
        if constexpr (!std::is_lvalue_reference_v<value_type&&> && !std::is_const_v<std::remove_reference_t<value_type>>) {
            if (model_value_rebind_access::owns(value, std::addressof(target))) {
                throw_model_move_source_owns_destination();
            }
        }
        target.emplace(detail::rebind_model_value(std::forward<value_type>(value)));
    }
}

template <naming_convention model_naming, typename... descriptor_types>
class model_storage;

struct json_member_key_size_sink final {
    std::size_t size_{0};

    constexpr void push_back(char) noexcept {
        ++size_;
    }
    constexpr void append(std::string_view text) noexcept {
        size_ += text.size();
    }
};

template <std::size_t capacity>
struct json_member_key_buffer final {
    std::array<char, capacity> bytes_{};
    std::size_t size_{0};

    constexpr void push_back(char value) noexcept {
        bytes_[size_++] = value;
    }
    constexpr void append(std::string_view text) noexcept {
        for (const char value : text) {
            push_back(value);
        }
    }
};

template <fixed_string wire_name>
[[nodiscard]] consteval std::size_t json_member_key_size() {
    json_member_key_size_sink sink;
    return append_json_member_key(sink, wire_name.view()) ? sink.size_ : 0;
}

template <fixed_string wire_name>
[[nodiscard]] consteval std::array<char, json_member_key_size<wire_name>()> make_json_member_key() {
    json_member_key_buffer<json_member_key_size<wire_name>()> key;
    (void)append_json_member_key(key, wire_name.view());
    return key.bytes_;
}

template <typename field_value_type, bool is_required, typename options_type, fixed_string field_wire_name>
class model_field final {
    static_assert(std::is_empty_v<options_type>, "model field options must be stateless metadata");
    static_assert(json_member_key_size<field_wire_name>() != 0, "Ruvia model wire names must be well-formed UTF-8");
    static_assert(!(is_required && options_type::has_default), "RUVIA_DEFAULT applies only to a missing optional field; declare it with RUVIA_OPTIONAL_FIELD");
    static_assert(!options_type::stringify_int64 || std::is_same_v<field_value_type, std::int64_t> || std::is_same_v<field_value_type, std::uint64_t>, "RUVIA_STRINGIFY_INT64 requires std::int64_t or std::uint64_t");
    static_assert(!options_type::decimal_number || detail::is_ruvia_decimal<field_value_type>, "RUVIA_DECIMAL_NUMBER requires ruvia::decimal");
    static_assert(!options_type::has_bytes_codec || detail::is_ruvia_bytes<field_value_type> || (detail::is_ruvia_array<field_value_type> && [] {
        if constexpr (detail::is_ruvia_array<field_value_type>) {
            return detail::is_ruvia_bytes<typename field_value_type::value_type>;
        }
        return false;
    }()),
        "RUVIA_BYTES_* requires ruvia::bytes or ruvia::array<ruvia::bytes>");

public:
    using value_type = field_value_type;
    using field_options_type = options_type;
    static constexpr bool required = is_required;
    static constexpr bool nullable = options_type::nullable;
    static constexpr bool guaranteed_value = (is_required || options_type::has_default) && !options_type::nullable;
    static constexpr bytes_encoding binary_encoding = options_type::binary_encoding;
    static constexpr bool stringify_int64 = options_type::stringify_int64;
    static constexpr bool decimal_number = options_type::decimal_number;

    constexpr model_field() noexcept = default;

    [[nodiscard]] static constexpr std::string_view static_wire_name() noexcept {
        return field_wire_name.view();
    }

    [[nodiscard]] constexpr std::string_view wire_name() const noexcept {
        return field_wire_name.view();
    }

    [[nodiscard]] static constexpr bool matches_wire_or_alias(std::string_view key, bool case_insensitive = false) noexcept {
        if (case_insensitive ? http_ascii_equals_ignore_case(key, field_wire_name.view()) : (key == field_wire_name.view())) {
            return true;
        }
        return options_type::matches_alias(key, case_insensitive);
    }

    [[nodiscard]] static constexpr std::string_view json_member_key(bool leading_comma) noexcept {
        const std::string_view key(json_member_key_.data(), json_member_key_.size());
        return leading_comma ? key : key.substr(1);
    }

    [[nodiscard]] detail::model_field_state state() const noexcept {
        return state_;
    }

    [[nodiscard]] bool is_present() const noexcept {
        return present_;
    }

    [[nodiscard]] bool is_null() const noexcept {
        return state_ == detail::model_field_state::null;
    }

    [[nodiscard]] bool has_value() const noexcept {
        return value_.has_value();
    }

    [[nodiscard]] const std::optional<field_value_type>& value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }
    [[nodiscard]] const std::optional<field_value_type>& value() const&& = delete;

    [[nodiscard]] const field_value_type& direct_value() const& RUVIA_LIFETIMEBOUND
        requires guaranteed_value
    {
        if (!value_) {
            throw std::logic_error("model field has no value");
        }
        return *value_;
    }
    [[nodiscard]] const field_value_type& direct_value() const&&
        requires guaranteed_value
    = delete;

    [[nodiscard]] field_value_type& ensure() & RUVIA_LIFETIMEBOUND
        requires(!detail::is_ruvia_json_value<field_value_type>)
    {
        if (!value_) {
            value_.emplace();
        }
        state_ = detail::model_field_state::parsed;
        return *value_;
    }
    field_value_type& ensure() &&
        requires(!detail::is_ruvia_json_value<field_value_type>)
    = delete;

    [[nodiscard]] field_value_type take_direct_value() &&
        requires guaranteed_value
    {
        if (!value_) {
            throw std::logic_error("model field has no value");
        }
        auto taken = detail::model_value_rebind_access::own(std::move(*value_));
        reset();
        return taken;
    }

    [[nodiscard]] std::optional<field_value_type> take_value() && {
        if (!value_) {
            reset();
            return std::nullopt;
        }
        std::optional<field_value_type> taken(detail::model_value_rebind_access::own(std::move(*value_)));
        reset();
        return taken;
    }

    template <typename input_type>
    void assign(input_type&& input) {
        if constexpr (std::is_null_pointer_v<std::remove_cvref_t<input_type>>) {
            static_assert(nullable, "setting null requires RUVIA_NULLABLE");
            assign_null();
        } else {
            if constexpr (detail::is_ruvia_json_value<input_type>) {
                if (input.is_null()) {
                    if constexpr (nullable) {
                        assign_null();
                        return;
                    } else {
                        throw std::invalid_argument("setting JSON null requires RUVIA_NULLABLE");
                    }
                }
            }
            assign_field_value(value_, std::forward<input_type>(input));
            state_ = detail::model_field_state::parsed;
        }
    }

    void set_null() noexcept
        requires nullable
    {
        assign_null();
    }

    void reset() noexcept {
        state_ = detail::model_field_state::missing;
        value_.reset();
    }

    void apply_initial() {
        options_type::apply_initial([this]<typename input_type>(input_type&& value) {
            this->assign(std::forward<input_type>(value));
        });
    }

    void apply_default() {
        if constexpr (options_type::has_default) {
            if (state_ != detail::model_field_state::missing) {
                return;
            }
            options_type::apply_default([this]<typename input_type>(input_type&& value) {
                this->assign(std::forward<input_type>(value));
            });
        }
    }

    [[nodiscard]] constexpr bool emit_null() const noexcept {
        return options_type::emit_null();
    }

    [[nodiscard]] constexpr bool omit_empty() const noexcept {
        return options_type::omit_empty();
    }

    friend bool operator==(const model_field& left, const model_field& right) {
        if (left.state_ != right.state_) {
            return false;
        }
        if (left.state_ == detail::model_field_state::parsed) {
            return left.value_ == right.value_;
        }
        return true;
    }

    void mark_duplicate() noexcept {
        present_ = true;
        state_ = detail::model_field_state::duplicate;
    }

    void mark_invalid_type() noexcept {
        present_ = true;
        state_ = detail::model_field_state::invalid_type;
    }

    void mark_element_invalid() noexcept {
        present_ = true;
        state_ = detail::model_field_state::element_invalid;
    }

    void mark_present_parsed() noexcept {
        present_ = true;
        state_ = detail::model_field_state::parsed;
    }

    void mark_null() noexcept {
        present_ = true;
        assign_null();
    }

private:
    template <naming_convention, typename...>
    friend class model_storage;
    friend struct ::ruvia::detail::model_value_factory;

    void assign_null() noexcept {
        value_.reset();
        state_ = detail::model_field_state::null;
    }

    void emplace_parsed(field_value_type&& value) {
        value_.emplace(std::move(value));
        present_ = true;
        state_ = detail::model_field_state::parsed;
    }

    void own_in_place() {
        if (value_) {
            model_value_rebind_access::own_in_place(*value_);
        }
    }

    [[nodiscard]] model_field rebind() const& {
        model_field rebound;
        rebound.state_ = state_;
        rebound.present_ = present_;
        if (value_) {
            rebound.value_.emplace(detail::rebind_model_value(*value_));
        }
        return rebound;
    }

    [[nodiscard]] model_field rebind() && {
        own_in_place();
        model_field rebound;
        rebound.state_ = state_;
        rebound.present_ = present_;
        rebound.value_ = std::move(value_);
        reset_after_move();
        return rebound;
    }

    [[nodiscard]] bool owns(const void* address) const noexcept {
        return value_ && model_value_rebind_access::owns(*value_, address);
    }

    void reset_after_move() noexcept {
        present_ = false;
        value_.reset();
        state_ = detail::model_field_state::missing;
    }

    static constexpr auto json_member_key_ = make_json_member_key<field_wire_name>();
    detail::model_field_state state_{detail::model_field_state::missing};
    bool present_{false};
    std::optional<field_value_type> value_;
};

template <typename t_type>
using field_option_tuple_type = std::conditional_t<is_model_option<t_type>(), std::tuple<t_type>, std::tuple<>>;

template <typename t_type>
using field_rule_tuple_type = std::conditional_t<is_validation_rule<t_type>(), std::tuple<t_type>, std::tuple<>>;

template <typename tuple_type>
struct tuple_model_options;

template <typename... ts_type>
struct tuple_model_options<std::tuple<ts_type...>> {
    using type = model_options<ts_type...>;
};

template <typename tuple_type>
struct tuple_rules;

template <typename... ts_type>
struct tuple_rules<std::tuple<ts_type...>> {
    using type = rules<ts_type...>;
};

template <fixed_string field_source_name, fixed_string field_wire_name, bool is_explicit_wire, typename field_value_type, bool is_required, typename... arg_types>
struct model_field_descriptor final {
    static_assert(((is_model_option<arg_types>() || is_validation_rule<arg_types>()) && ... && true),
        "RUVIA_REQUIRED_FIELD/RUVIA_OPTIONAL_FIELD accept model options and validation rules");

    using value_type = field_value_type;
    using options_type = typename tuple_model_options<decltype(std::tuple_cat(std::tuple<>{}, std::declval<field_option_tuple_type<arg_types>>()...))>::type;
    using rules_type = typename tuple_rules<decltype(std::tuple_cat(std::tuple<>{}, std::declval<field_rule_tuple_type<arg_types>>()...))>::type;

    template <naming_convention model_naming>
    static constexpr auto effective_wire_name_value = [] {
        if constexpr (is_explicit_wire) {
            return field_wire_name;
        } else {
            return ::ruvia::detail::apply_naming_convention<field_source_name, model_naming>();
        }
    }();

    template <naming_convention model_naming>
    static constexpr auto effective_wire_name() noexcept {
        return effective_wire_name_value<model_naming>;
    }

    template <naming_convention model_naming>
    using field_for_naming = model_field<field_value_type, is_required, options_type, effective_wire_name_value<model_naming>>;

    static constexpr auto source_name = field_source_name;
    static constexpr auto wire_name = field_wire_name;
    static constexpr bool explicit_wire = is_explicit_wire;
    static constexpr bool required = is_required;
    static constexpr bool nullable = options_type::nullable;
    static constexpr bool guaranteed_value = (is_required || options_type::has_default) && !options_type::nullable;
    static constexpr bool has_field_rules = !std::is_same_v<rules_type, rules<>>;

    static_assert(!has_field_rules || !(detail::is_ruvia_json_value<field_value_type> || detail::is_ruvia_json_object<field_value_type>), "json_value/json_object are dynamic content, not typed field validation schemas");
};

template <typename item_type>
[[nodiscard]] constexpr bool is_field_descriptor() noexcept {
    return !is_model_level_option<item_type>() && requires { typename std::remove_cvref_t<item_type>::options_type; };
}

template <typename item_type>
using model_descriptor_tuple_type = std::conditional_t<is_field_descriptor<item_type>(), std::tuple<item_type>, std::tuple<>>;

template <typename item_type>
using model_validator_tuple_type = std::conditional_t<is_model_level_validate<item_type>(), std::tuple<item_type>, std::tuple<>>;

template <typename... item_types>
struct extract_model_schema;

template <typename... descriptor_types>
struct tuple_to_model_schema;
template <typename... descriptor_types>
struct tuple_to_model_schema<std::tuple<descriptor_types...>> {
    using type = model_schema<descriptor_types...>;
};

template <typename... item_types>
struct extract_model_schema {
    static_assert(((is_field_descriptor<item_types>() || is_model_level_option<item_types>()) && ... && true),
        "RUVIA_MODEL arguments must be field descriptors, RUVIA_MODEL_NAMING, or RUVIA_MODEL_VALIDATE");
    static_assert(((is_model_level_naming<item_types>() ? 1 : 0) + ... + 0) <= 1, "RUVIA_MODEL accepts at most one RUVIA_MODEL_NAMING");

    template <typename t_type>
    [[nodiscard]] static consteval naming_convention one_naming() noexcept {
        if constexpr (is_model_level_naming<t_type>()) {
            return t_type::convention;
        }
        return naming_convention::identity;
    }

    [[nodiscard]] static consteval naming_convention resolve_naming() noexcept {
        naming_convention resolved = naming_convention::identity;
        ((is_model_level_naming<item_types>() ? (resolved = one_naming<item_types>(), 0) : 0), ...);
        return resolved;
    }

    static constexpr naming_convention naming = resolve_naming();
    using descriptors_tuple = decltype(std::tuple_cat(std::tuple<>{}, std::declval<model_descriptor_tuple_type<item_types>>()...));
    using schema_type = typename tuple_to_model_schema<descriptors_tuple>::type;
    using validators_tuple = decltype(std::tuple_cat(std::tuple<>{}, std::declval<model_validator_tuple_type<item_types>>()...));
};

template <typename tuple_type, naming_convention naming>
struct make_model_storage_from_tuple;
template <naming_convention naming, typename... descriptor_types>
struct make_model_storage_from_tuple<std::tuple<descriptor_types...>, naming> {
    using type = model_storage<naming, descriptor_types...>;
};

template <typename... descriptor_types>
consteval void validate_model_schema(model_schema<descriptor_types...>) {
    static_assert((detail::is_model_field<typename descriptor_types::value_type> && ...), "model fields must use Ruvia values or nested models");
}

template <fixed_string field, typename... descriptor_types>
[[nodiscard]] consteval std::size_t model_field_index() {
    static_assert((std::size_t{field == descriptor_types::source_name} + ... + 0) == 1, "unknown or duplicate Ruvia model field");
    const std::array<std::string_view, sizeof...(descriptor_types)> names{descriptor_types::source_name.view()...};
    return fixed_name_index(names, field.view());
}

template <typename... descriptor_types>
[[nodiscard]] consteval bool unique_model_field_names() {
    const std::array<std::string_view, sizeof...(descriptor_types)> names{descriptor_types::source_name.view()...};
    return fixed_names_distinct(names);
}

template <naming_convention model_naming, typename... descriptor_types>
[[nodiscard]] consteval bool unique_model_wire_and_alias_names() {
    std::size_t total = 0;
    ((total += 1, descriptor_types::options_type::for_each_alias([&total](std::string_view) {
        ++total;
    })),
        ...);
    std::vector<std::string_view> all;
    all.reserve(total);
    bool valid_utf8 = true;
    ((all.push_back(descriptor_types::template effective_wire_name_value<model_naming>.view()),
         descriptor_types::options_type::for_each_alias([&](std::string_view alias) {
        if (alias.empty() || !is_valid_utf8(alias)) {
            valid_utf8 = false;
        }
        all.push_back(alias);
    })),
        ...);
    if (!valid_utf8) {
        return false;
    }
    for (std::size_t left = 0; left < all.size(); ++left) {
        for (std::size_t right = left + 1; right < all.size(); ++right) {
            if (all[left] == all[right]) {
                return false;
            }
        }
    }
    return true;
}

template <naming_convention model_naming, typename... descriptor_types>
[[nodiscard]] consteval bool unique_model_wire_names_ignore_ascii_case_for(model_schema<descriptor_types...>) {
    std::vector<std::string_view> all;
    ((all.push_back(descriptor_types::template effective_wire_name_value<model_naming>.view()),
         descriptor_types::options_type::for_each_alias([&](std::string_view alias) {
        all.push_back(alias);
    })),
        ...);
    for (std::size_t left = 0; left < all.size(); ++left) {
        for (std::size_t right = left + 1; right < all.size(); ++right) {
            if (http_ascii_equals_ignore_case(all[left], all[right])) {
                return false;
            }
        }
    }
    return true;
}

template <typename model_type>
[[nodiscard]] consteval bool unique_model_wire_names_ignore_ascii_case() {
    using clean_type = std::remove_cvref_t<model_type>;
    return unique_model_wire_names_ignore_ascii_case_for<clean_type::ruvia_model_naming>(typename clean_type::ruvia_model_schema_type{});
}

template <std::size_t index, typename descriptor_type, typename... remaining_types, typename model_type, typename visitor_type>
[[nodiscard]] bool visit_model_field_by_wire_name_impl(model_type& model, std::string_view wire_name, bool& visit_result, visitor_type& visitor) {
    auto& slot = model_access::slot<index>(model);
    using slot_type = std::remove_cvref_t<decltype(slot)>;
    if (slot_type::matches_wire_or_alias(wire_name, false)) {
        visit_result = visitor(slot);
        return true;
    }
    if constexpr (sizeof...(remaining_types) > 0) {
        return visit_model_field_by_wire_name_impl<index + 1, remaining_types...>(model, wire_name, visit_result, visitor);
    } else {
        return false;
    }
}

template <typename model_type, typename... descriptor_types, typename visitor_type>
[[nodiscard]] bool visit_model_field_by_wire_name(model_type& model, model_schema<descriptor_types...>, std::string_view wire_name, bool& visit_result, visitor_type&& visitor) {
    if constexpr (sizeof...(descriptor_types) == 0) {
        return false;
    } else {
        auto& visitor_ref = visitor;
        return visit_model_field_by_wire_name_impl<0, descriptor_types...>(model, wire_name, visit_result, visitor_ref);
    }
}

template <typename model_type>
void initialize_model(model_type& value) {
    visit_model_fields(value, model_access::schema<model_type>(), [](const auto&, auto& slot) {
        slot.apply_initial();
    });
}

}  // namespace ruvia::detail::model

namespace ruvia::detail {

struct form_body_input final {
    std::string_view body_;
    model_string_storage string_storage_{model_string_storage::borrowed};
};

struct request_fields_input final {
    std::span<const http_header_view> fields_;
    bool case_insensitive_names_{false};
};

using model_input = std::variant<form_body_input, request_fields_input>;

template <typename visitor_type>
    requires std::invocable<visitor_type&, std::string_view, std::string_view>
void visit_form_body_fields(std::string_view body, visitor_type&& visitor) {
    auto& visitor_ref = visitor;
    std::string decoded_name;
    (void)ruvia::visit_url_encoded_pairs(body, [&](std::string_view name, std::string_view value) {
        visitor_ref(*ruvia::decode_url_component(name, decoded_name, url_decode_mode::form), value);
    });
}

enum class form_value_encoding : std::uint8_t {
    url_encoded,
    decoded,
};

[[nodiscard]] inline std::optional<bool> parse_form_bool(std::string_view decoded) noexcept {
    if (decoded == "true" || decoded == "1") {
        return true;
    }
    if (decoded == "false" || decoded == "0") {
        return false;
    }
    return std::nullopt;
}

template <typename number_type>
[[nodiscard]] std::optional<number_type> parse_form_number(std::string_view decoded) {
    if (decoded.empty()) {
        return std::nullopt;
    }
    number_type parsed_value{};
    if constexpr (std::is_floating_point_v<number_type>) {
        const auto value = detail::parse_decimal_number<number_type>(decoded);
        if (!value || !std::isfinite(*value)) {
            return std::nullopt;
        }
        parsed_value = *value;
    } else {
        const auto integer = parse_integer<number_type>(decoded);
        if (!integer) {
            return std::nullopt;
        }
        parsed_value = *integer;
    }
    return parsed_value;
}

[[nodiscard]] inline std::string_view decode_form_value(std::string_view input, form_value_encoding encoding, std::string& storage) {
    if (encoding == form_value_encoding::decoded) {
        return input;
    }
    return *ruvia::decode_url_component(input, storage, url_decode_mode::form);
}

template <typename t_type>
    requires is_form_scalar_or_text<t_type>
[[nodiscard]] std::optional<t_type> parse_form_value(std::string_view input, form_value_encoding encoding, model_string_storage string_storage) {
    using field_type = std::remove_cvref_t<t_type>;

    if constexpr (std::is_same_v<field_type, bool>) {
        std::string decoded_storage;
        return parse_form_bool(decode_form_value(input, encoding, decoded_storage));
    } else if constexpr (is_model_scalar<field_type>) {
        std::string decoded_storage;
        return parse_form_number<field_type>(decode_form_value(input, encoding, decoded_storage));
    } else if constexpr (is_ruvia_enum<field_type>) {
        std::string decoded_storage;
        return try_parse_enum<field_type>(decode_form_value(input, encoding, decoded_storage));
    } else if constexpr (is_ruvia_uuid<field_type>) {
        std::string decoded_storage;
        return uuid::try_parse(decode_form_value(input, encoding, decoded_storage));
    } else if constexpr (is_ruvia_decimal<field_type>) {
        std::string decoded_storage;
        return decimal::try_parse(decode_form_value(input, encoding, decoded_storage));
    } else if constexpr (is_chrono_timestamp<field_type>) {
        std::string decoded_storage;
        return try_parse_rfc3339<typename field_type::duration>(decode_form_value(input, encoding, decoded_storage));
    } else if constexpr (is_civil_date<field_type>) {
        std::string decoded_storage;
        return try_parse_civil_date(decode_form_value(input, encoding, decoded_storage));
    } else {
        std::string decoded_storage;
        if (decode_form_value(input, encoding, decoded_storage).data() != input.data()) {
            field_type value;
            value.assign(std::move(decoded_storage));
            return value;
        }
        if (string_storage == model_string_storage::owned) {
            return field_type(input);
        }
        return model_value_factory::make_borrowed_string(input);
    }
}

class json_parse_budget final {
public:
    explicit json_parse_budget(model_parse_options options = {}, std::vector<validation_issue>* issues = nullptr) noexcept :
        elements_(options.max_array_elements_),
        bytes_(options.max_representation_bytes_),
        reject_unknown_fields_(options.reject_unknown_fields_),
        issues_(issues) {}

    class path_scope final {
    public:
        path_scope(json_parse_budget& budget, const model::model_path& path) noexcept :
            budget_(budget),
            previous_(std::exchange(budget.path_, &path)) {}
        path_scope(const path_scope&) = delete;
        path_scope& operator=(const path_scope&) = delete;
        ~path_scope() {
            budget_.path_ = previous_;
        }

    private:
        json_parse_budget& budget_;
        const model::model_path* previous_;
    };

    [[nodiscard]] const model::model_path& path() const noexcept {
        return *path_;
    }

    [[nodiscard]] bool has_issue_sink() const noexcept {
        return issues_ != nullptr;
    }

    void add_issue(const model::model_path& target, std::string_view code, std::string_view message) {
        has_recorded_issues_ = true;
        if (issues_ != nullptr) {
            std::string rendered;
            target.render(rendered);
            issues_->push_back(validation_issue({.field_ = rendered, .code_ = code, .message_ = message}));
        }
    }

    void record_unknown_field() {
        if (!reject_unknown_fields_) {
            return;
        }
        unknown_rejected_ = true;
        add_issue(path(), "unknown_field", "is not declared");
    }

    [[nodiscard]] bool unknown_rejected() const noexcept {
        return unknown_rejected_;
    }

    [[nodiscard]] bool has_recorded_issues() const noexcept {
        return has_recorded_issues_;
    }

    [[nodiscard]] bool consume_bytes(std::size_t bytes_value) noexcept {
        if (bytes_value > bytes_) {
            exhausted_ = true;
            return false;
        }
        bytes_ -= bytes_value;
        return true;
    }

    [[nodiscard]] bool consume_owned_text(std::size_t bytes, std::size_t copies = 1) noexcept {
        return consume_owned_text_chunk(bytes, copies, true);
    }

    [[nodiscard]] bool consume_owned_text_chunk(std::size_t bytes, std::size_t copies, bool first_chunk) noexcept {
        const auto string_bytes = first_chunk ? sizeof(std::string) : std::size_t{0};
        if (bytes > (std::numeric_limits<std::size_t>::max() - string_bytes) / copies) {
            exhausted_ = true;
            return false;
        }
        return consume_bytes(bytes * copies + string_bytes);
    }

    [[nodiscard]] bool consume_element() noexcept {
        if (elements_ == 0) {
            exhausted_ = true;
            return false;
        }
        --elements_;
        return true;
    }

    [[nodiscard]] bool exhausted() const noexcept {
        return exhausted_;
    }

    [[nodiscard]] std::size_t element_checkpoint() const noexcept {
        return elements_;
    }

    void restore_elements(std::size_t checkpoint) noexcept {
        elements_ = checkpoint;
    }

    [[nodiscard]] bool skip_value(std::string_view& input, std::size_t depth) noexcept {
        auto consume = [this]() noexcept {
            return consume_element();
        };
        return skip_json_value(input, depth, consume);
    }

private:
    std::size_t elements_;
    std::size_t bytes_;
    bool exhausted_{false};
    bool reject_unknown_fields_;
    bool unknown_rejected_{false};
    bool has_recorded_issues_{false};
    std::vector<validation_issue>* issues_;
    model::model_path root_path_;
    const model::model_path* path_{&root_path_};
};

template <typename t_type, bytes_encoding bin_encoding = bytes_encoding::base64, bool allow_stringified_int64 = false>
[[nodiscard]] std::optional<t_type> parse_json_value(std::string_view& input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget, bool* element_had_issue = nullptr);

template <typename view_type>
[[nodiscard]] std::optional<view_type> parse_json_view_value(std::string_view& input, std::size_t depth, model_string_storage string_storage, bool require_object, json_parse_budget& budget);

struct model_parse_access final {
    template <typename model_type>
    [[nodiscard]] static std::optional<model_type> parse_value(std::string_view& input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget, std::string_view skip_discriminator = {}) {
        if (depth > max_json_depth) {
            return std::nullopt;
        }
        auto value = model::model_access::empty<model_type>();
        if (!materialize_json(value, input, depth, string_storage, budget, skip_discriminator)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename model_type>
    [[nodiscard]] static model_type parse_form(const model_input& input, std::vector<validation_issue>* issues = nullptr) {
        auto value = model::model_access::empty<model_type>();
        materialize_form(value, input, issues);
        return value;
    }

private:
    template <typename model_type>
    static void apply_defaults(model_type& value) {
        model::visit_model_fields(value, model::model_access::schema<model_type>(), [](const auto&, auto& slot) {
            slot.apply_default();
        });
    }

    template <typename slot_type>
    [[nodiscard]] static bool parse_json_field_slot(slot_type& slot, std::string_view& value_input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget) {
        if (slot.is_present()) {
            slot.mark_duplicate();
            return budget.skip_value(value_input, depth + 1);
        }
        using unwrapped_slot_type = std::remove_cvref_t<slot_type>;
        using value_type = typename unwrapped_slot_type::value_type;
        auto null_input = value_input;
        if (consume_json_literal(null_input, "null")) {
            value_input = null_input;
            if constexpr (unwrapped_slot_type::nullable) {
                slot.mark_null();
            } else {
                slot.mark_invalid_type();
            }
            return true;
        }
        const auto element_checkpoint = budget.element_checkpoint();
        bool element_had_issue = false;
        if (auto parsed_value = parse_json_value<value_type, unwrapped_slot_type::binary_encoding, unwrapped_slot_type::stringify_int64>(value_input, depth + 1, string_storage, budget, &element_had_issue); parsed_value) {
            if (element_had_issue) {
                slot.mark_element_invalid();
            } else {
                model_value_factory::emplace_parsed(slot, std::move(*parsed_value));
            }
            return true;
        }
        if (budget.exhausted()) {
            return false;
        }
        if (element_had_issue) {
            // Child/element parser already consumed the container and recorded the exact inner issue.
            slot.mark_element_invalid();
            return true;
        }
        budget.restore_elements(element_checkpoint);
        if (!budget.skip_value(value_input, depth + 1)) {
            return false;
        }
        slot.mark_invalid_type();
        return true;
    }

    template <typename model_type>
    [[nodiscard]] static bool visit_json_field(model_type& value, std::string_view key, std::string_view& value_input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget, std::string_view skip_discriminator) {
        if (!skip_discriminator.empty() && key == skip_discriminator) {
            return budget.skip_value(value_input, depth + 1);
        }
        bool field_result = true;
        const bool matched = model::visit_model_field_by_wire_name(value, model::model_access::schema<model_type>(), key, field_result, [&](auto& slot) -> bool {
            const model::model_path field_path(budget.path(), slot.wire_name());
            const json_parse_budget::path_scope scope(budget, field_path);
            return parse_json_field_slot(slot, value_input, depth, string_storage, budget);
        });
        if (matched) {
            return field_result;
        }
        const model::model_path unknown_path(budget.path(), key);
        const json_parse_budget::path_scope scope(budget, unknown_path);
        budget.record_unknown_field();
        return budget.skip_value(value_input, depth + 1);
    }

    template <typename model_type>
    static bool materialize_json(model_type& value, std::string_view& input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget, std::string_view skip_discriminator) {
        const auto visit_field = [&value, depth, string_storage, &budget, skip_discriminator](std::string_view key, std::string_view& value_input) -> bool {
            return visit_json_field(value, key, value_input, depth, string_storage, budget, skip_discriminator);
        };
        const auto consume_key_allocation = [&budget](std::size_t bytes_value, bool first_chunk) noexcept {
            return budget.consume_owned_text_chunk(bytes_value, 1, first_chunk);
        };
        const bool valid = consume_json_object_fields(input, depth, visit_field, consume_key_allocation);
        if (valid && !budget.unknown_rejected() && !budget.has_recorded_issues() && model_validation_access::structure_valid(value)) {
            apply_defaults(value);
        }
        return valid;
    }

    template <typename slot_type>
    static void bind_form_field_slot(slot_type& slot, std::string_view field_value, form_value_encoding encoding, model_string_storage string_storage, std::vector<validation_issue>* issues) {
        using value_type = typename std::remove_cvref_t<slot_type>::value_type;
        if constexpr (is_ruvia_array<value_type> && is_form_field<value_type>) {
            using elem_type = typename value_type::value_type;
            if (slot.state() == model_field_state::invalid_type || slot.state() == model_field_state::element_invalid) {
                return;
            }
            const std::size_t elem_index = slot.value() ? slot.value()->size() : 0;
            auto parsed_elem = parse_form_value<elem_type>(field_value, encoding, string_storage);
            if (parsed_elem) {
                slot.ensure().push_back(std::move(*parsed_elem));
                slot.mark_present_parsed();
                return;
            }
            if (issues != nullptr) {
                const model::model_path root;
                const model::model_path field_p(root, slot.wire_name());
                const model::model_path item_p(field_p, elem_index);
                std::string rendered;
                item_p.render(rendered);
                issues->push_back(validation_issue({.field_ = rendered, .code_ = "invalid_type", .message_ = model::expected_type_name<elem_type>()}));
                slot.mark_element_invalid();
            } else {
                slot.mark_invalid_type();
            }
            return;
        }
        if (slot.is_present()) {
            slot.mark_duplicate();
            return;
        }
        if constexpr (is_form_scalar_or_text<value_type>) {
            auto parsed_value = parse_form_value<value_type>(field_value, encoding, string_storage);
            if (parsed_value) {
                model_value_factory::emplace_parsed(slot, std::move(*parsed_value));
                return;
            }
        }
        slot.mark_invalid_type();
    }

    template <typename model_type>
    static void bind_form_field(model_type& value, std::string_view key, std::string_view field_value, form_value_encoding encoding, model_string_storage string_storage, std::vector<validation_issue>* issues) {
        bool field_result = true;
        (void)model::visit_model_field_by_wire_name(value, model::model_access::schema<model_type>(), key, field_result, [&](auto& slot) -> bool {
            bind_form_field_slot(slot, field_value, encoding, string_storage, issues);
            return true;
        });
    }

    template <typename model_type>
    static void bind_form_field_ignore_case(model_type& value, std::string_view key, std::string_view field_value, std::vector<validation_issue>* issues) {
        bool matched = false;
        model::visit_model_fields(value, model::model_access::schema<model_type>(), [&](const auto&, auto& slot) {
            using slot_type = std::remove_cvref_t<decltype(slot)>;
            if (matched || !slot_type::matches_wire_or_alias(key, true)) {
                return;
            }
            matched = true;
            bind_form_field_slot(slot, field_value, form_value_encoding::decoded, model_string_storage::borrowed, issues);
        });
    }

    template <typename model_type>
    static void materialize_form(model_type& value, const model_input& input, std::vector<validation_issue>* issues) {
        if (const auto* body = std::get_if<form_body_input>(&input)) {
            visit_form_body_fields(body->body_, [&value, string_storage = body->string_storage_, issues](std::string_view key, std::string_view field_value) {
                bind_form_field(value, key, field_value, form_value_encoding::url_encoded, string_storage, issues);
            });
        } else {
            const auto& fields_value = std::get<request_fields_input>(input);
            for (const auto& field : fields_value.fields_) {
                if (fields_value.case_insensitive_names_) {
                    bind_form_field_ignore_case(value, field.name(), field.value(), issues);
                } else {
                    bind_form_field(value, field.name(), field.value(), form_value_encoding::decoded, model_string_storage::borrowed, issues);
                }
            }
        }
        if (model_validation_access::structure_valid(value)) {
            apply_defaults(value);
        }
    }
};

template <typename sequence_type>
struct json_sequence_value_traits;

template <typename element_type>
struct json_sequence_value_traits<array<element_type>> {
    using value_type = element_type;

    static void emplace(array<element_type>& value, element_type&& element) {
        model_value_factory::emplace_parsed(value, std::move(element));
    }
};

template <typename element_type>
struct json_sequence_value_traits<boxed_array<element_type>> {
    using value_type = element_type;

    static void emplace(boxed_array<element_type>& value, element_type&& element) {
        model_value_factory::emplace_parsed(value, std::move(element));
    }
};

template <typename sequence_type, bytes_encoding bin_encoding = bytes_encoding::base64>
[[nodiscard]] std::optional<sequence_type> parse_json_sequence_value(std::string_view& input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget, bool* element_had_issue) {
    using traits_type = json_sequence_value_traits<std::remove_cvref_t<sequence_type>>;
    using element_type = typename traits_type::value_type;

    sequence_type value;
    std::size_t index = 0;
    bool any_elem_issue = false;
    const bool consumed = consume_json_array_elements(input, depth, [&](std::string_view& element_input) {
        if (!budget.consume_element()) {
            return false;
        }
        constexpr auto element_bytes = 4 * (sizeof(element_type) + sizeof(element_type*));
        if (!budget.consume_bytes(element_bytes)) {
            return false;
        }
        const model::model_path item_path(budget.path(), index++);
        const json_parse_budget::path_scope scope(budget, item_path);
        const auto elem_checkpoint = budget.element_checkpoint();
        bool inner_issue = false;
        auto element = parse_json_value<element_type, bin_encoding>(element_input, depth + 1, string_storage, budget, &inner_issue);
        if (element.has_value()) {
            if (inner_issue) {
                any_elem_issue = true;
            }
            traits_type::emplace(value, std::move(*element));
            return true;
        }
        if (budget.exhausted()) {
            return false;
        }
        if (inner_issue) {
            any_elem_issue = true;
            return true;
        }
        if (!budget.has_issue_sink()) {
            return false;
        }
        budget.restore_elements(elem_checkpoint);
        if (!budget.skip_value(element_input, depth + 1)) {
            return false;
        }
        budget.add_issue(item_path, "invalid_type", model::expected_type_name<element_type, bin_encoding>());
        any_elem_issue = true;
        return true;
    });
    if (!consumed) {
        return std::nullopt;
    }
    if (element_had_issue != nullptr && any_elem_issue) {
        *element_had_issue = true;
    }
    if (any_elem_issue && element_had_issue == nullptr) {
        return std::nullopt;
    }
    return value;
}

template <typename map_type>
[[nodiscard]] std::optional<map_type> parse_json_map_value(std::string_view& input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget, bool* element_had_issue) {
    using v_type = typename std::remove_cvref_t<map_type>::mapped_type;
    map_type value;
    bool any_elem_issue = false;
    const auto consume_key_allocation = [&budget](std::size_t bytes_value, bool first_chunk) noexcept {
        return budget.consume_owned_text_chunk(bytes_value, 1, first_chunk);
    };
    const bool consumed = consume_json_object_fields(input, depth, [&](std::string_view key, std::string_view& val_input) -> bool {
        if (!budget.consume_element()) {
            return false;
        }
        constexpr auto entry_bytes = 2 * sizeof(typename map_type::entry_type);
        if (!budget.consume_bytes(entry_bytes)) {
            return false;
        }
        const model::model_path entry_path(budget.path(), key);
        const json_parse_budget::path_scope scope(budget, entry_path);
        if (value.contains(key)) {
            if (!budget.has_issue_sink()) {
                return false;
            }
            if (!budget.skip_value(val_input, depth + 1)) {
                return false;
            }
            budget.add_issue(entry_path, "duplicate", "is duplicated");
            any_elem_issue = true;
            return true;
        }
        const auto elem_checkpoint = budget.element_checkpoint();
        bool inner_issue = false;
        auto parsed_val = parse_json_value<v_type>(val_input, depth + 1, string_storage, budget, &inner_issue);
        if (!parsed_val.has_value()) {
            if (budget.exhausted()) {
                return false;
            }
            if (inner_issue) {
                any_elem_issue = true;
                return true;
            }
            if (!budget.has_issue_sink()) {
                return false;
            }
            budget.restore_elements(elem_checkpoint);
            if (!budget.skip_value(val_input, depth + 1)) {
                return false;
            }
            budget.add_issue(entry_path, "invalid_type", model::expected_type_name<v_type>());
            any_elem_issue = true;
            return true;
        }
        if (inner_issue) {
            any_elem_issue = true;
        }
        if (!budget.consume_owned_text(key.size(), 1)) {
            return false;
        }
        (void)model_value_factory::emplace_parsed_map(value, string(key), std::move(*parsed_val));
        return true;
    }, consume_key_allocation);

    if (!consumed) {
        return std::nullopt;
    }
    if (element_had_issue != nullptr && any_elem_issue) {
        *element_had_issue = true;
    }
    if (any_elem_issue && element_had_issue == nullptr) {
        return std::nullopt;
    }
    return value;
}

template <typename variant_type>
[[nodiscard]] std::optional<variant_type> parse_json_tagged_variant(std::string_view& input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget, bool* element_had_issue);

template <typename t_type, bytes_encoding bin_encoding, bool allow_stringified_int64>
[[nodiscard]] std::optional<t_type> parse_json_value(std::string_view& input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget, bool* element_had_issue) {
    using field_type = std::remove_cvref_t<t_type>;
    if (depth > max_json_depth || budget.exhausted()) {
        return std::nullopt;
    }
    auto remaining = input;
    if constexpr (is_ruvia_string<field_type>) {
        std::string decoded;
        bool started = false;
        const auto token = parse_json_string(remaining, &decoded, [&](std::size_t chunk_bytes) noexcept {
            const bool first = !std::exchange(started, true);
            return budget.consume_owned_text_chunk(chunk_bytes, 2, first);
        });
        if (!token.has_value()) {
            return std::nullopt;
        }
        if (token->encoding() == json_string_encoding::escaped) {
            input = remaining;
            field_type value;
            value.assign(std::move(decoded));
            return value;
        }
        if (string_storage == model_string_storage::borrowed) {
            input = remaining;
            return model_value_factory::make_borrowed_string(token->raw());
        }
        if (!budget.consume_owned_text(token->raw().size(), 1)) {
            return std::nullopt;
        }
        input = remaining;
        return field_type(token->raw());
    } else if constexpr (is_ruvia_bytes<field_type>) {
        std::string decoded;
        bool started = false;
        const auto token = parse_json_string(remaining, &decoded, [&](std::size_t chunk_bytes) noexcept {
            const bool first = !std::exchange(started, true);
            return budget.consume_owned_text_chunk(chunk_bytes, 2, first);
        });
        if (!token || (token->encoding() == json_string_encoding::literal && !budget.consume_owned_text(token->raw().size(), 1))) {
            return std::nullopt;
        }
        auto value = decode_model_binary(json_string_text(*token, decoded), bin_encoding);
        if (!value) {
            return std::nullopt;
        }
        input = remaining;
        return value;
    } else if constexpr (is_ruvia_uuid<field_type>) {
        std::string decoded;
        bool started = false;
        const auto token = parse_json_string(remaining, &decoded, [&](std::size_t chunk_bytes) noexcept {
            const bool first = !std::exchange(started, true);
            return budget.consume_owned_text_chunk(chunk_bytes, 2, first);
        });
        if (!token) {
            return std::nullopt;
        }
        const auto parsed = uuid::try_parse(json_string_text(*token, decoded));
        if (!parsed) {
            return std::nullopt;
        }
        input = remaining;
        return *parsed;
    } else if constexpr (is_ruvia_decimal<field_type>) {
        skip_json_whitespace(remaining);
        if (!remaining.empty() && remaining.front() == '"') {
            std::string decoded;
            bool started = false;
            const auto token = parse_json_string(remaining, &decoded, [&](std::size_t chunk_bytes) noexcept {
                const bool first = !std::exchange(started, true);
                return budget.consume_owned_text_chunk(chunk_bytes, 1, first);
            });
            if (!token) {
                return std::nullopt;
            }
            const auto parsed = decimal::try_parse(json_string_text(*token, decoded));
            if (!parsed) {
                return std::nullopt;
            }
            input = remaining;
            return *parsed;
        }
        const auto num_token = scan_json_number_token(remaining);
        if (num_token.length_ == 0) {
            return std::nullopt;
        }
        const auto parsed = decimal::try_parse(remaining.substr(0, num_token.length_));
        if (!parsed) {
            return std::nullopt;
        }
        remaining.remove_prefix(num_token.length_);
        input = remaining;
        return *parsed;
    } else if constexpr (is_chrono_timestamp<field_type>) {
        std::string decoded;
        bool started = false;
        const auto token = parse_json_string(remaining, &decoded, [&](std::size_t chunk_bytes) noexcept {
            const bool first = !std::exchange(started, true);
            return budget.consume_owned_text_chunk(chunk_bytes, 2, first);
        });
        if (!token) {
            return std::nullopt;
        }
        const auto parsed = try_parse_rfc3339<typename field_type::duration>(json_string_text(*token, decoded));
        if (!parsed) {
            return std::nullopt;
        }
        input = remaining;
        return *parsed;
    } else if constexpr (is_civil_date<field_type>) {
        std::string decoded;
        bool started = false;
        const auto token = parse_json_string(remaining, &decoded, [&](std::size_t chunk_bytes) noexcept {
            const bool first = !std::exchange(started, true);
            return budget.consume_owned_text_chunk(chunk_bytes, 2, first);
        });
        if (!token) {
            return std::nullopt;
        }
        const auto parsed = try_parse_civil_date(json_string_text(*token, decoded));
        if (!parsed) {
            return std::nullopt;
        }
        input = remaining;
        return *parsed;
    } else if constexpr (is_ruvia_enum<field_type>) {
        std::string decoded;
        bool started = false;
        const auto token = parse_json_string(remaining, &decoded, [&](std::size_t chunk_bytes) noexcept {
            const bool first = !std::exchange(started, true);
            return budget.consume_owned_text_chunk(chunk_bytes, 2, first);
        });
        if (!token) {
            return std::nullopt;
        }
        const auto parsed = try_parse_enum<field_type>(json_string_text(*token, decoded));
        if (!parsed) {
            return std::nullopt;
        }
        input = remaining;
        return *parsed;
    } else if constexpr (is_ruvia_json_value<field_type> || is_ruvia_json_object<field_type>) {
        return parse_json_view_value<field_type>(input, depth, string_storage, is_ruvia_json_object<field_type>, budget);
    } else if constexpr (is_ruvia_array<field_type> || is_ruvia_boxed_array<field_type>) {
        auto parsed_value = parse_json_sequence_value<field_type, bin_encoding>(remaining, depth, string_storage, budget, element_had_issue);
        if (!parsed_value.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return parsed_value;
    } else if constexpr (is_ruvia_map<field_type>) {
        auto parsed_value = parse_json_map_value<field_type>(remaining, depth, string_storage, budget, element_had_issue);
        if (!parsed_value.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return parsed_value;
    } else if constexpr (is_tagged_variant<field_type>) {
        auto parsed_value = parse_json_tagged_variant<field_type>(remaining, depth, string_storage, budget, element_had_issue);
        if (!parsed_value.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return parsed_value;
    } else if constexpr (std::is_same_v<field_type, bool>) {
        if (consume_json_literal(remaining, "true")) {
            input = remaining;
            return true;
        }
        if (consume_json_literal(remaining, "false")) {
            input = remaining;
            return false;
        }
        return std::nullopt;
    } else if constexpr (is_model_scalar<field_type>) {
        if constexpr (allow_stringified_int64) {
            skip_json_whitespace(remaining);
            if (!remaining.empty() && remaining.front() == '"') {
                std::string decoded;
                bool started = false;
                const auto token = parse_json_string(remaining, &decoded, [&](std::size_t chunk_bytes) noexcept {
                    const bool first = !std::exchange(started, true);
                    return budget.consume_owned_text_chunk(chunk_bytes, 1, first);
                });
                if (!token) {
                    return std::nullopt;
                }
                const auto parsed_int = parse_integer<field_type>(json_string_text(*token, decoded));
                if (!parsed_int) {
                    return std::nullopt;
                }
                input = remaining;
                return *parsed_int;
            }
        }
        field_type parsed_value{};
        if (!parse_json_number_value(remaining, parsed_value)) {
            return std::nullopt;
        }
        input = remaining;
        return parsed_value;
    } else if constexpr (is_model<field_type>) {
        auto nested = model_parse_access::parse_value<field_type>(remaining, depth, string_storage, budget);
        if (!nested.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return nested;
    } else {
        static_assert(always_false<field_type>, "RUVIA_MODEL JSON field type is not supported");
    }
}

template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_complete_json_value(std::string_view input, model_string_storage string_storage, model_parse_options options = {}, std::vector<validation_issue>* issues = nullptr, bool* had_recorded_issues = nullptr) {
    json_parse_budget budget(options, issues);
    if (!budget.consume_bytes(sizeof(t_type))) {
        return std::nullopt;
    }
    bool element_had_issue = false;
    auto value = parse_json_value<t_type>(input, 0, string_storage, budget, issues != nullptr ? &element_had_issue : nullptr);
    skip_json_whitespace(input);
    if (!value || !input.empty() || budget.exhausted()) {
        return std::nullopt;
    }
    if (had_recorded_issues != nullptr) {
        *had_recorded_issues = budget.unknown_rejected() || budget.has_recorded_issues() || element_had_issue;
    }
    if (budget.unknown_rejected() && issues == nullptr) {
        return std::nullopt;
    }
    return value;
}

template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_json_document(std::string_view input, model_string_storage string_storage, model_parse_options options = {}) {
    bool had_issues = false;
    auto value = parse_complete_json_value<t_type>(input, string_storage, options, nullptr, &had_issues);
    if (!value || had_issues || !model_validation_access::value_structure_valid(*value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] inline bool is_complete_json_value(std::string_view body, model_parse_options options, bool require_object) noexcept {
    json_parse_budget budget(options);
    skip_json_whitespace(body);
    if (require_object && (body.empty() || body.front() != '{')) {
        return false;
    }
    if (!budget.skip_value(body, 0)) {
        return false;
    }
    skip_json_whitespace(body);
    return body.empty();
}

struct model_json_access final {
    template <typename model_type>
    [[nodiscard]] static std::size_t size_hint(const model_type& model);

    template <typename model_type>
    static void append(std::string& output, const model_type& model);

    template <typename model_type>
    static void append_members_only(std::string& output, const model_type& model, bool leading_comma_started);
};

template <typename value_type>
consteval std::size_t json_fixed_scalar_size_hint() noexcept {
    using t_type = std::remove_cvref_t<value_type>;
    if constexpr (std::is_same_v<t_type, bool>) {
        return 5;
    } else if constexpr (is_model_integer<t_type>) {
        return static_cast<std::size_t>(std::numeric_limits<t_type>::digits10) + 5;
    } else if constexpr (is_model_floating<t_type>) {
        return 32;
    } else if constexpr (is_ruvia_uuid<t_type>) {
        return 38;
    } else if constexpr (is_ruvia_decimal<t_type>) {
        return 34;
    } else if constexpr (is_chrono_timestamp<t_type>) {
        return 32;
    } else if constexpr (is_civil_date<t_type>) {
        return 12;
    } else {
        return 0;
    }
}

template <typename value_type, bytes_encoding bin_encoding = bytes_encoding::base64>
[[nodiscard]] std::size_t json_size_hint_value(const value_type& value) {
    using t_type = std::remove_cvref_t<value_type>;
    if constexpr (json_fixed_scalar_size_hint<t_type>() != 0) {
        return json_fixed_scalar_size_hint<t_type>();
    } else if constexpr (is_ruvia_enum<t_type>) {
        const auto formatted = try_format_enum(value);
        return formatted ? json_string_size_hint(*formatted) : 16;
    } else if constexpr (is_ruvia_bytes<t_type>) {
        if constexpr (bin_encoding == bytes_encoding::hex) {
            return value.size() * 2 + 2;
        } else {
            return base64_encoded_size(value.size(), bin_encoding == bytes_encoding::base64url ? base64_padding::forbidden : base64_padding::required) + 2;
        }
    } else if constexpr (is_ruvia_string<t_type>) {
        return json_string_size_hint(value.view());
    } else if constexpr (is_model<t_type>) {
        return model_json_access::size_hint(value);
    } else if constexpr (is_tagged_variant<t_type>) {
        return value.json_size_hint();
    } else if constexpr (is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type>) {
        return value.view().size();
    } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
        constexpr std::size_t item_hint = [] {
            if constexpr (is_ruvia_array<t_type>) {
                return json_fixed_scalar_size_hint<typename t_type::value_type>();
            } else {
                return std::size_t{0};
            }
        }();
        if constexpr (item_hint != 0) {
            return value.empty() ? 2 : 2 + value.size() * item_hint + (value.size() - 1);
        } else {
            std::size_t size = 2;
            bool first = true;
            for (const auto& item : value) {
                if (!first) {
                    ++size;
                }
                first = false;
                size += json_size_hint_value<typename t_type::value_type, bin_encoding>(item);
            }
            return size;
        }
    } else if constexpr (is_ruvia_map<t_type>) {
        std::size_t size = 2;
        bool first = true;
        for (const auto& entry : value) {
            if (!first) {
                ++size;
            }
            first = false;
            size += json_string_size_hint(entry.key_.view()) + 1 + json_size_hint_value(entry.value_);
        }
        return size;
    } else {
        static_assert(always_false<t_type>, "JSON output must use Ruvia model types");
    }
}

template <typename value_type, bytes_encoding bin_encoding = bytes_encoding::base64, bool stringify_i64 = false, bool decimal_num = false>
void append_json_value(std::string& output, const value_type& value);

template <typename sequence_type, bytes_encoding bin_encoding = bytes_encoding::base64>
void append_json_sequence(std::string& output, const sequence_type& value) {
    output.push_back('[');
    bool first = true;
    for (const auto& item : value) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        append_json_value<typename sequence_type::value_type, bin_encoding>(output, item);
    }
    output.push_back(']');
}

template <typename map_type>
void append_json_map(std::string& output, const map_type& value) {
    output.push_back('{');
    bool first = true;
    for (const auto& entry : value) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        append_json_string(output, entry.key_.view());
        output.push_back(':');
        append_json_value(output, entry.value_);
    }
    output.push_back('}');
}

template <typename value_type, bytes_encoding bin_encoding, bool stringify_i64, bool decimal_num>
void append_json_value(std::string& output, const value_type& value) {
    using t_type = std::remove_cvref_t<value_type>;
    if constexpr (std::is_same_v<t_type, bool>) {
        output.append(value ? "true" : "false");
    } else if constexpr (is_model_integer<t_type>) {
        if constexpr (stringify_i64) {
            output.push_back('"');
            append_decimal(output, value);
            output.push_back('"');
        } else {
            append_decimal(output, value);
        }
    } else if constexpr (is_model_floating<t_type>) {
        if (!std::isfinite(value)) {
            output.append("null");
            return;
        }
        append_number(output, value, "JSON number cannot be formatted");
    } else if constexpr (is_ruvia_bytes<t_type>) {
        append_model_binary(output, value.view(), bin_encoding);
    } else if constexpr (is_ruvia_uuid<t_type>) {
        output.push_back('"');
        value.append_to(output);
        output.push_back('"');
    } else if constexpr (is_ruvia_decimal<t_type>) {
        if constexpr (decimal_num) {
            value.append_to(output);
        } else {
            output.push_back('"');
            value.append_to(output);
            output.push_back('"');
        }
    } else if constexpr (is_chrono_timestamp<t_type>) {
        output.push_back('"');
        append_rfc3339(output, value);
        output.push_back('"');
    } else if constexpr (is_civil_date<t_type>) {
        output.push_back('"');
        append_civil_date(output, value);
        output.push_back('"');
    } else if constexpr (is_ruvia_enum<t_type>) {
        append_json_string(output, to_enum_string(value));
    } else if constexpr (is_ruvia_string<t_type>) {
        append_json_string(output, value.view());
    } else if constexpr (is_model<t_type>) {
        model_json_access::append(output, value);
    } else if constexpr (is_tagged_variant<t_type>) {
        value.append_json(output);
    } else if constexpr (is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type>) {
        output.append(value.view());
    } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
        append_json_sequence<t_type, bin_encoding>(output, value);
    } else if constexpr (is_ruvia_map<t_type>) {
        append_json_map(output, value);
    } else {
        static_assert(always_false<t_type>, "JSON output must use Ruvia model types");
    }
}

namespace model {

enum class serialized_field_kind {
    value,
    null,
};

template <typename model_type, typename visitor_type>
void visit_serialized_fields(const model_type& value, visitor_type&& visitor, bool already_started = false) {
    bool first = !already_started;
    visit_model_fields(value, model_access::schema<model_type>(), [&](const auto&, const auto& slot) {
        using slot_type = std::remove_cvref_t<decltype(slot)>;
        const auto& field_value = slot.value();
        if constexpr (slot_type::required) {
            if (!field_value && !(slot_type::nullable && slot.is_null())) {
                throw std::logic_error("cannot serialize unset required model field");
            }
        }
        serialized_field_kind kind;
        if (field_value && !(slot.omit_empty() && is_empty_value(*field_value))) {
            kind = serialized_field_kind::value;
        } else if (!field_value && (slot.is_null() || slot.emit_null())) {
            kind = serialized_field_kind::null;
        } else {
            return;
        }
        visitor(slot, kind, !first);
        first = false;
    });
}

}  // namespace model

template <typename model_type>
std::size_t model_json_access::size_hint(const model_type& value) {
    std::size_t size = 2;
    model::visit_serialized_fields(value, [&size](const auto& slot, auto kind, bool comma) {
        using slot_type = std::remove_cvref_t<decltype(slot)>;
        size += slot.json_member_key(comma).size();
        size += kind == model::serialized_field_kind::value ? json_size_hint_value<typename slot_type::value_type, slot_type::binary_encoding>(*slot.value()) : 4;
    });
    return size;
}

template <typename model_type>
void model_json_access::append_members_only(std::string& output, const model_type& value, bool leading_comma_started) {
    model::visit_serialized_fields(value, [&output](const auto& slot, auto kind, bool comma) {
        using slot_type = std::remove_cvref_t<decltype(slot)>;
        output.append(slot.json_member_key(comma));
        if (kind == model::serialized_field_kind::value) {
            append_json_value<typename slot_type::value_type, slot_type::binary_encoding, slot_type::stringify_int64, slot_type::decimal_number>(output, *slot.value());
        } else {
            output.append("null");
        }
    }, leading_comma_started);
}

template <typename model_type>
void model_json_access::append(std::string& output, const model_type& value) {
    output.push_back('{');
    append_members_only(output, value, false);
    output.push_back('}');
}

}  // namespace ruvia::detail

namespace ruvia {

namespace detail {
template <typename variant_type>
struct variant_discriminator;
}  // namespace detail

// Internally tagged discriminated union of RUVIA_MODEL types (`{"type":"card", ...}`).
template <fixed_string discriminator_field, typename... case_types>
class tagged_variant final {
    static_assert(sizeof...(case_types) > 0, "tagged_variant requires at least one tagged_case");
    static_assert((detail::is_model<typename case_types::type> && ...), "tagged_case alternatives must be RUVIA_MODEL types");
    static_assert(detail::model::json_member_key_size<discriminator_field>() != 0, "tagged_variant discriminator must be well-formed UTF-8");

    [[nodiscard]] static consteval bool valid_cases() {
        const std::array<std::string_view, sizeof...(case_types)> tags{case_types::tag.view()...};
        for (const auto t : tags) {
            if (t.empty() || !is_valid_utf8(t)) {
                return false;
            }
        }
        return detail::fixed_names_distinct(tags);
    }
    static_assert(valid_cases(), "tagged_variant case tags must be non-empty, well-formed UTF-8, and distinct");

    template <typename target_type>
    [[nodiscard]] static consteval std::size_t case_index_of() {
        constexpr std::size_t matches = (std::size_t{std::is_same_v<target_type, typename case_types::type>} + ... + 0);
        static_assert(matches == 1, "type is not a unique alternative of this tagged_variant");
        std::size_t found = 0;
        std::size_t current = 0;
        ((std::is_same_v<target_type, typename case_types::type> ? (found = current, 0) : (++current, 0)), ...);
        return found;
    }

public:
    tagged_variant() = default;
    tagged_variant(const tagged_variant&) = delete;
    tagged_variant& operator=(const tagged_variant&) = delete;
    tagged_variant(tagged_variant&&) noexcept = default;

    tagged_variant& operator=(tagged_variant&& other) {
        if (this == &other) {
            return *this;
        }
        if (other.owns_for_model(this)) {
            detail::throw_model_move_source_owns_destination();
        }
        auto rebound = std::move(other).rebind_for_model();
        value_ = std::move(rebound.value_);
        return *this;
    }

    template <typename alt_type>
        requires((std::is_same_v<std::remove_cvref_t<alt_type>, typename case_types::type> || ...))
    explicit tagged_variant(alt_type&& alt) :
        value_(std::in_place_index<case_index_of<std::remove_cvref_t<alt_type>>()>, detail::rebind_model_value(std::forward<alt_type>(alt))) {}

    [[nodiscard]] tagged_variant clone() const {
        return rebind_for_model();
    }

    [[nodiscard]] std::size_t index() const noexcept {
        return value_.index();
    }

    [[nodiscard]] std::string_view tag() const noexcept {
        static constexpr std::array<std::string_view, sizeof...(case_types)> tags{case_types::tag.view()...};
        return tags[value_.index()];
    }

    template <typename alt_type>
    [[nodiscard]] bool is() const noexcept {
        return value_.index() == case_index_of<alt_type>();
    }

    template <typename alt_type>
    [[nodiscard]] alt_type* get_if() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<case_index_of<alt_type>()>(&value_);
    }
    template <typename alt_type>
    [[nodiscard]] const alt_type* get_if() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<case_index_of<alt_type>()>(&value_);
    }
    template <typename alt_type>
    const alt_type* get_if() const&& = delete;

    template <typename alt_type>
    [[nodiscard]] alt_type& get() & RUVIA_LIFETIMEBOUND {
        return std::get<case_index_of<alt_type>()>(value_);
    }
    template <typename alt_type>
    [[nodiscard]] const alt_type& get() const& RUVIA_LIFETIMEBOUND {
        return std::get<case_index_of<alt_type>()>(value_);
    }
    template <typename alt_type>
    const alt_type& get() const&& = delete;

    template <typename alt_type, typename... args_type>
    alt_type& emplace(args_type&&... args) & RUVIA_LIFETIMEBOUND {
        return value_.template emplace<case_index_of<alt_type>()>(std::forward<args_type>(args)...);
    }
    template <typename alt_type, typename... args_type>
    alt_type& emplace(args_type&&...) && = delete;

    template <typename visitor_type>
    decltype(auto) visit(visitor_type&& visitor) & {
        return std::visit(std::forward<visitor_type>(visitor), value_);
    }
    template <typename visitor_type>
    decltype(auto) visit(visitor_type&& visitor) const& {
        return std::visit(std::forward<visitor_type>(visitor), value_);
    }
    template <typename visitor_type>
    void visit(visitor_type&&) const&& = delete;

    friend bool operator==(const tagged_variant& left, const tagged_variant& right) = default;

private:
    friend struct detail::model_value_rebind_access;
    friend struct detail::variant_discriminator<tagged_variant>;
    template <typename v_type, bytes_encoding, bool, bool>
    friend void detail::append_json_value(std::string&, const v_type&);
    template <typename v_type, bytes_encoding>
    friend std::size_t detail::json_size_hint_value(const v_type&);
    template <typename v_type>
    friend std::optional<v_type> detail::parse_json_tagged_variant(std::string_view&, std::size_t, detail::model_string_storage, detail::json_parse_budget&, bool*);

    static constexpr auto discriminator_key_ = detail::model::make_json_member_key<discriminator_field>();

    [[nodiscard]] std::size_t json_size_hint() const {
        return visit([this](const auto& alt) {
            return detail::model_json_access::size_hint(alt) + discriminator_key_.size() + json_string_size_hint(tag());
        });
    }

    void append_json(std::string& output) const {
        output.push_back('{');
        const std::string_view disc_key(discriminator_key_.data() + 1, discriminator_key_.size() - 1);
        output.append(disc_key);
        append_json_string(output, tag());
        visit([&output](const auto& alt) {
            detail::model_json_access::append_members_only(output, alt, true);
        });
        output.push_back('}');
    }

    template <std::size_t idx = 0>
    [[nodiscard]] static tagged_variant rebind_index(const tagged_variant& src) {
        if (src.value_.index() == idx) {
            tagged_variant out(empty_tag{}, std::in_place_index<idx>, detail::rebind_model_value(std::get<idx>(src.value_)));
            return out;
        }
        if constexpr (idx + 1 < sizeof...(case_types)) {
            return rebind_index<idx + 1>(src);
        } else {
            std::terminate();
        }
    }

    struct empty_tag final {};
    template <std::size_t idx, typename arg_type>
    tagged_variant(empty_tag, std::in_place_index_t<idx>, arg_type&& arg) :
        value_(std::in_place_index<idx>, std::forward<arg_type>(arg)) {}

    [[nodiscard]] tagged_variant rebind_for_model() const& {
        return rebind_index(*this);
    }

    void own_for_model() {
        visit([](auto& alt) {
            detail::model_value_rebind_access::own_in_place(alt);
        });
    }

    [[nodiscard]] tagged_variant rebind_for_model() && {
        own_for_model();
        return std::move(*this);
    }

    [[nodiscard]] bool owns_for_model(const void* address) const noexcept {
        return visit([address](const auto& alt) noexcept {
            return detail::model_value_rebind_access::owns(alt, address);
        });
    }

    using variant_type = std::variant<typename case_types::type...>;
    variant_type value_{};
};

}  // namespace ruvia

namespace ruvia::detail {

template <std::size_t idx, fixed_string discriminator, typename case_type, typename... rest_types>
[[nodiscard]] std::optional<tagged_variant<discriminator, case_type, rest_types...>> parse_tagged_case_dispatch(std::size_t target_idx, std::string_view& object_input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget);

template <typename variant_type>
struct variant_discriminator;

template <fixed_string discriminator, typename... case_types>
struct variant_discriminator<tagged_variant<discriminator, case_types...>> {
    using variant_type = tagged_variant<discriminator, case_types...>;
    static constexpr std::string_view name = discriminator.view();

    template <std::size_t idx, typename case_type, typename... rest_types>
    [[nodiscard]] static std::optional<variant_type> parse_case_impl(std::string_view tag_text, std::string_view& object_input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget, bool& matched_tag) {
        if (tag_text == case_type::tag.view()) {
            matched_tag = true;
            auto parsed_alt = model_parse_access::parse_value<typename case_type::type>(object_input, depth, string_storage, budget, name);
            if (!parsed_alt) {
                return std::nullopt;
            }
            return variant_type(typename variant_type::empty_tag{}, std::in_place_index<idx>, std::move(*parsed_alt));
        }
        if constexpr (sizeof...(rest_types) > 0) {
            return parse_case_impl<idx + 1, rest_types...>(tag_text, object_input, depth, string_storage, budget, matched_tag);
        } else {
            return std::nullopt;
        }
    }

    [[nodiscard]] static std::optional<variant_type> parse_by_tag(std::string_view tag_text, std::string_view& object_input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget, bool& matched_tag) {
        return parse_case_impl<0, case_types...>(tag_text, object_input, depth, string_storage, budget, matched_tag);
    }
};

template <typename variant_type>
[[nodiscard]] std::optional<variant_type> parse_json_tagged_variant(std::string_view& input, std::size_t depth, model_string_storage string_storage, json_parse_budget& budget, bool* element_had_issue) {
    if (depth > max_json_depth) {
        return std::nullopt;
    }
    auto probe = input;
    skip_json_whitespace(probe);
    if (probe.empty() || probe.front() != '{') {
        return std::nullopt;
    }
    const auto object_begin = probe;
    const auto elem_checkpoint = budget.element_checkpoint();
    if (!budget.skip_value(probe, depth)) {
        return std::nullopt;
    }
    budget.restore_elements(elem_checkpoint);
    const auto raw_object = object_begin.substr(0, object_begin.size() - probe.size());

    constexpr auto disc_name = variant_discriminator<variant_type>::name;
    const model::model_path disc_path(budget.path(), disc_name);

    std::size_t disc_count = 0;
    std::string_view disc_raw_value{};
    auto scan_cursor = raw_object;
    const bool scanned = consume_json_object_fields(scan_cursor, depth, [&](std::string_view key, std::string_view& val_cursor) -> bool {
        const auto v_start = val_cursor;
        if (!skip_json_value(val_cursor, depth + 1)) {
            return false;
        }
        if (key == disc_name) {
            ++disc_count;
            disc_raw_value = v_start.substr(0, v_start.size() - val_cursor.size());
        }
        return true;
    });
    if (!scanned) {
        return std::nullopt;
    }
    if (disc_count == 0) {
        if (!budget.has_issue_sink()) {
            return std::nullopt;
        }
        budget.add_issue(disc_path, "required", "is required");
        if (element_had_issue != nullptr) {
            *element_had_issue = true;
        }
        input = probe;
        return variant_type{};
    }
    if (disc_count > 1) {
        if (!budget.has_issue_sink()) {
            return std::nullopt;
        }
        budget.add_issue(disc_path, "duplicate", "is duplicated");
        if (element_had_issue != nullptr) {
            *element_had_issue = true;
        }
        input = probe;
        return variant_type{};
    }
    std::string decoded_tag;
    auto tag_cursor = disc_raw_value;
    const auto tag_token = parse_json_string(tag_cursor, &decoded_tag);
    if (!tag_token) {
        if (!budget.has_issue_sink()) {
            return std::nullopt;
        }
        budget.add_issue(disc_path, "invalid_type", "must be a string");
        if (element_had_issue != nullptr) {
            *element_had_issue = true;
        }
        input = probe;
        return variant_type{};
    }
    const auto tag_text = json_string_text(*tag_token, decoded_tag);
    auto parse_cursor = raw_object;
    bool matched_tag = false;
    auto parsed = variant_discriminator<variant_type>::parse_by_tag(tag_text, parse_cursor, depth, string_storage, budget, matched_tag);
    if (!matched_tag) {
        if (!budget.has_issue_sink()) {
            return std::nullopt;
        }
        budget.add_issue(disc_path, "one_of", "unknown discriminator tag");
        if (element_had_issue != nullptr) {
            *element_had_issue = true;
        }
        input = probe;
        return variant_type{};
    }
    if (!parsed) {
        return std::nullopt;
    }
    input = probe;
    return parsed;
}

}  // namespace ruvia::detail

namespace ruvia::detail::model {

template <naming_convention model_naming, typename... descriptor_types>
class model_storage final {
    using field_tuple = std::tuple<typename descriptor_types::template field_for_naming<model_naming>...>;

public:
    static constexpr bool has_initial = (descriptor_types::options_type::has_initial || ... || false);

    model_storage() noexcept {
        static_assert(unique_model_field_names<descriptor_types...>(), "Ruvia model source field names must be unique");
        static_assert(unique_model_wire_and_alias_names<model_naming, descriptor_types...>(), "Ruvia model wire and alias names must be unique and well-formed UTF-8");
    }

    model_storage(const model_storage&) = delete;
    model_storage& operator=(const model_storage&) = delete;

    model_storage(model_storage&& other) noexcept :
        fields_(std::move(other.fields_)) {
        reset_moved_fields(other.fields_);
    }

    friend bool operator==(const model_storage& left, const model_storage& right) {
        return left.fields_ == right.fields_;
    }

    model_storage& operator=(model_storage&& other) {
        if (this == &other) {
            return *this;
        }
        if (::ruvia::detail::model_address_within(this, std::addressof(other), sizeof(model_storage)) || other.owns(this)) {
            ::ruvia::detail::throw_model_move_source_owns_destination();
        }
        auto rebound = std::move(other).rebind();
        replace_fields(fields_, std::move(rebound.fields_));
        return *this;
    }

    template <fixed_string field>
    [[nodiscard]] decltype(auto) get() const& RUVIA_LIFETIMEBOUND {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        using slot_type = std::tuple_element_t<index, field_tuple>;
        const auto& slot = std::get<index>(fields_);
        if constexpr (slot_type::guaranteed_value) {
            return slot.direct_value();
        } else {
            return slot.value();
        }
    }

    template <fixed_string field>
    [[nodiscard]] decltype(auto) get() const&& = delete;

    template <fixed_string field>
    [[nodiscard]] const auto& optional() const& noexcept RUVIA_LIFETIMEBOUND {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        return std::get<index>(fields_).value();
    }

    template <fixed_string field>
    const auto& optional() const&& = delete;

    template <fixed_string field>
    [[nodiscard]] decltype(auto) take() && {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        using slot_type = std::tuple_element_t<index, field_tuple>;
        auto& slot = std::get<index>(fields_);
        if constexpr (slot_type::guaranteed_value) {
            return std::move(slot).take_direct_value();
        } else {
            return std::move(slot).take_value();
        }
    }

    template <fixed_string field>
    [[nodiscard]] bool is_present() const noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        return std::get<index>(fields_).is_present();
    }

    template <fixed_string field>
    [[nodiscard]] bool is_null() const noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        return std::get<index>(fields_).is_null();
    }

    template <fixed_string field>
    [[nodiscard]] bool has_value() const noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        return std::get<index>(fields_).has_value();
    }

    template <fixed_string field, typename value_type>
    void set(value_type&& value) & {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        std::get<index>(fields_).assign(std::forward<value_type>(value));
    }

    template <fixed_string field>
        requires(std::tuple_element_t<model_field_index<field, descriptor_types...>(), std::tuple<descriptor_types...>>::nullable)
    void set_null() & noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        std::get<index>(fields_).set_null();
    }

    template <fixed_string field>
        requires requires(std::tuple_element_t<model_field_index<field, descriptor_types...>(), field_tuple>& slot) { slot.ensure(); }
    [[nodiscard]] decltype(auto) ensure() & RUVIA_LIFETIMEBOUND {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        return std::get<index>(fields_).ensure();
    }

    template <fixed_string field>
        requires(!std::tuple_element_t<model_field_index<field, descriptor_types...>(), std::tuple<descriptor_types...>>::required)
    void reset() & noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        std::get<index>(fields_).reset();
    }

    template <std::size_t index>
    [[nodiscard]] decltype(auto) slot() & noexcept {
        return std::get<index>(fields_);
    }

    template <std::size_t index>
    [[nodiscard]] decltype(auto) slot() const& noexcept {
        return std::get<index>(fields_);
    }

    [[nodiscard]] model_storage rebind() const& {
        model_storage rebound;
        auto fields_value = rebind_fields(fields_);
        replace_fields(rebound.fields_, std::move(fields_value));
        return rebound;
    }

    void own_in_place() {
        std::apply([](auto&... field) {
            (field.own_in_place(), ...);
        }, fields_);
    }

    [[nodiscard]] model_storage rebind() && {
        own_in_place();
        return model_storage(std::move(*this));
    }

    [[nodiscard]] bool owns(const void* address) const noexcept {
        return std::apply([address](const auto&... field) {
            return (field.owns(address) || ... || false);
        }, fields_);
    }

private:
    template <std::size_t... indices>
    [[nodiscard]] static field_tuple rebind_fields(const field_tuple& source_value, std::index_sequence<indices...>) {
        return field_tuple{std::get<indices>(source_value).rebind()...};
    }

    [[nodiscard]] static field_tuple rebind_fields(const field_tuple& source_value) {
        return rebind_fields(source_value, std::index_sequence_for<descriptor_types...>{});
    }

    static void replace_fields(field_tuple& destination, field_tuple&& source_value) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<field_tuple>);
        std::destroy_at(std::addressof(destination));
        std::construct_at(std::addressof(destination), std::move(source_value));
    }

    template <std::size_t... indices>
    static void reset_moved_fields(field_tuple& fields_value, std::index_sequence<indices...>) noexcept {
        (std::get<indices>(fields_value).reset_after_move(), ...);
    }

    static void reset_moved_fields(field_tuple& fields_value) noexcept {
        reset_moved_fields(fields_value, std::index_sequence_for<descriptor_types...>{});
    }

    field_tuple fields_;
};

}  // namespace ruvia::detail::model

// Field options & codecs.
#define RUVIA_DEFAULT(value)                     \
    ::ruvia::detail::model::default_value<[]() { \
        return value;                            \
    }>
#define RUVIA_INITIAL(value)               \
    ::ruvia::detail::model::initial<[]() { \
        return value;                      \
    }>
#define RUVIA_NULLABLE ::ruvia::detail::model::nullable
#define RUVIA_OMIT_EMPTY ::ruvia::detail::model::omit_empty
#define RUVIA_EMIT_NULL ::ruvia::detail::model::emit_null
#define RUVIA_STRINGIFY_INT64 ::ruvia::detail::model::stringify_int64
#define RUVIA_DECIMAL_NUMBER ::ruvia::detail::model::decimal_as_number
#define RUVIA_BYTES_BASE64 ::ruvia::detail::model::bytes_codec_option<::ruvia::bytes_encoding::base64>
#define RUVIA_BYTES_BASE64URL ::ruvia::detail::model::bytes_codec_option<::ruvia::bytes_encoding::base64url>
#define RUVIA_BYTES_HEX ::ruvia::detail::model::bytes_codec_option<::ruvia::bytes_encoding::hex>
#define RUVIA_ALIAS(...) ::ruvia::detail::model::field_aliases<__VA_ARGS__>

// Model-level options inside RUVIA_MODEL(...).
#define RUVIA_MODEL_NAMING(convention) ::ruvia::detail::model::model_naming_option<convention>
#define RUVIA_MODEL_VALIDATE(fn)                                        \
    ::ruvia::detail::model::model_validate_option<                      \
        [](const auto& ruvia_model_self, auto& ruvia_model_validator) { \
        (fn)(ruvia_model_self, ruvia_model_validator);                  \
    }>

// Field declarations.
#define RUVIA_REQUIRED_FIELD(field, type, ...) ::ruvia::detail::model::model_field_descriptor<::ruvia::fixed_string{#field}, ::ruvia::fixed_string{#field}, false, type, true __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_REQUIRED_FIELD_NAME(wire_name, field, type, ...) ::ruvia::detail::model::model_field_descriptor<::ruvia::fixed_string{#field}, ::ruvia::fixed_string{wire_name}, true, type, true __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_OPTIONAL_FIELD(field, type, ...) ::ruvia::detail::model::model_field_descriptor<::ruvia::fixed_string{#field}, ::ruvia::fixed_string{#field}, false, type, false __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_OPTIONAL_FIELD_NAME(wire_name, field, type, ...) ::ruvia::detail::model::model_field_descriptor<::ruvia::fixed_string{#field}, ::ruvia::fixed_string{wire_name}, true, type, false __VA_OPT__(, ) __VA_ARGS__>

#define RUVIA_MODEL(t_type, ...)                                                                                                                                       \
    struct t_type final {                                                                                                                                              \
    private:                                                                                                                                                           \
        using extracted_type = ::ruvia::detail::model::extract_model_schema<__VA_ARGS__>;                                                                              \
        using storage_type = typename ::ruvia::detail::model::make_model_storage_from_tuple<typename extracted_type::descriptors_tuple, extracted_type::naming>::type; \
                                                                                                                                                                       \
    public:                                                                                                                                                            \
        [[maybe_unused]] static constexpr ::ruvia::naming_convention ruvia_model_naming = extracted_type::naming;                                                      \
        using ruvia_model_schema_type = typename extracted_type::schema_type;                                                                                          \
        using ruvia_model_validators_type = typename extracted_type::validators_tuple;                                                                                 \
        template <typename model_type = t_type>                                                                                                                        \
        t_type() noexcept(!storage_type::has_initial) {                                                                                                                \
            ::ruvia::detail::model::validate_model_schema(typename model_type::ruvia_model_schema_type{});                                                             \
            if constexpr (storage_type::has_initial) {                                                                                                                 \
                ::ruvia::detail::model::initialize_model(*this);                                                                                                       \
            }                                                                                                                                                          \
        }                                                                                                                                                              \
        t_type(const t_type&) = delete;                                                                                                                                \
        t_type& operator=(const t_type&) = delete;                                                                                                                     \
        t_type(t_type&&) noexcept = default;                                                                                                                           \
        t_type& operator=(t_type&&) = default;                                                                                                                         \
        [[nodiscard]] t_type clone() const {                                                                                                                           \
            return t_type(fields_.rebind());                                                                                                                           \
        }                                                                                                                                                              \
        [[maybe_unused]] friend bool operator==(const t_type&, const t_type&) = default;                                                                               \
        template <::ruvia::fixed_string field>                                                                                                                         \
        [[nodiscard]] decltype(auto) get() const& RUVIA_LIFETIMEBOUND {                                                                                                \
            return fields_.template get<field>();                                                                                                                      \
        }                                                                                                                                                              \
        template <::ruvia::fixed_string field>                                                                                                                         \
        [[nodiscard]] decltype(auto) get() const&& = delete;                                                                                                           \
        template <::ruvia::fixed_string field>                                                                                                                         \
        [[nodiscard]] const auto& optional() const& noexcept RUVIA_LIFETIMEBOUND {                                                                                     \
            return fields_.template optional<field>();                                                                                                                 \
        }                                                                                                                                                              \
        template <::ruvia::fixed_string field>                                                                                                                         \
        const auto& optional() const&& = delete;                                                                                                                       \
        template <::ruvia::fixed_string field>                                                                                                                         \
        [[nodiscard]] decltype(auto) take() && {                                                                                                                       \
            return std::move(fields_).template take<field>();                                                                                                          \
        }                                                                                                                                                              \
        template <::ruvia::fixed_string field>                                                                                                                         \
        [[nodiscard]] bool is_present() const noexcept {                                                                                                               \
            return fields_.template is_present<field>();                                                                                                               \
        }                                                                                                                                                              \
        template <::ruvia::fixed_string field>                                                                                                                         \
        [[nodiscard]] bool is_null() const noexcept {                                                                                                                  \
            return fields_.template is_null<field>();                                                                                                                  \
        }                                                                                                                                                              \
        template <::ruvia::fixed_string field>                                                                                                                         \
        [[nodiscard]] bool has_value() const noexcept {                                                                                                                \
            return fields_.template has_value<field>();                                                                                                                \
        }                                                                                                                                                              \
        template <::ruvia::fixed_string field, typename value_type>                                                                                                    \
        t_type& set(value_type&& value) & RUVIA_LIFETIMEBOUND {                                                                                                        \
            fields_.template set<field>(std::forward<value_type>(value));                                                                                              \
            return *this;                                                                                                                                              \
        }                                                                                                                                                              \
        template <::ruvia::fixed_string field, typename value_type>                                                                                                    \
        t_type& set(value_type&&) && = delete;                                                                                                                         \
        template <::ruvia::fixed_string field>                                                                                                                         \
            requires requires(storage_type& storage) { storage.template set_null<field>(); }                                                                           \
        t_type& set_null() & noexcept RUVIA_LIFETIMEBOUND {                                                                                                            \
            fields_.template set_null<field>();                                                                                                                        \
            return *this;                                                                                                                                              \
        }                                                                                                                                                              \
        template <::ruvia::fixed_string field>                                                                                                                         \
            requires requires(storage_type& storage) { storage.template set_null<field>(); }                                                                           \
        t_type& set_null() && = delete;                                                                                                                                \
        template <::ruvia::fixed_string field>                                                                                                                         \
            requires requires(storage_type& storage) { storage.template ensure<field>(); }                                                                             \
        [[nodiscard]] decltype(auto) ensure() & RUVIA_LIFETIMEBOUND {                                                                                                  \
            return fields_.template ensure<field>();                                                                                                                   \
        }                                                                                                                                                              \
        template <::ruvia::fixed_string field>                                                                                                                         \
            requires requires(storage_type& storage) { storage.template ensure<field>(); }                                                                             \
        [[nodiscard]] decltype(auto) ensure() && = delete;                                                                                                             \
        template <::ruvia::fixed_string field>                                                                                                                         \
            requires requires(storage_type& storage) { storage.template reset<field>(); }                                                                              \
        t_type& reset() & noexcept RUVIA_LIFETIMEBOUND {                                                                                                               \
            fields_.template reset<field>();                                                                                                                           \
            return *this;                                                                                                                                              \
        }                                                                                                                                                              \
        template <::ruvia::fixed_string field>                                                                                                                         \
            requires requires(storage_type& storage) { storage.template reset<field>(); }                                                                              \
        t_type& reset() && = delete;                                                                                                                                   \
                                                                                                                                                                       \
    private:                                                                                                                                                           \
        friend struct ::ruvia::detail::model::model_access;                                                                                                            \
        friend struct ::ruvia::detail::model_value_rebind_access;                                                                                                      \
        template <typename model_type = t_type>                                                                                                                        \
        explicit t_type(::ruvia::detail::model::empty_model_tag) noexcept {                                                                                            \
            ::ruvia::detail::model::validate_model_schema(typename model_type::ruvia_model_schema_type{});                                                             \
        }                                                                                                                                                              \
        explicit t_type(storage_type&& fields) noexcept :                                                                                                              \
            fields_(std::move(fields)) {}                                                                                                                              \
        storage_type fields_;                                                                                                                                          \
    }

namespace ruvia {

template <typename t_type>
class json_field_lookup final {
public:
    json_field_lookup(const json_field_lookup&) = delete;
    json_field_lookup(json_field_lookup&&) = default;
    json_field_lookup& operator=(const json_field_lookup&) = delete;
    json_field_lookup& operator=(json_field_lookup&&) = delete;

    [[nodiscard]] unique_lookup_status status() const noexcept {
        return status_;
    }

    [[nodiscard]] const std::optional<t_type>& value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }
    const std::optional<t_type>& value() const&& = delete;

    [[nodiscard]] std::optional<t_type> take_value() && noexcept(std::is_nothrow_move_constructible_v<t_type>) {
        return std::move(value_);
    }

private:
    friend class json_object;
    friend class json_value;

    explicit json_field_lookup(unique_lookup_status status) noexcept :
        status_(status) {}

    explicit json_field_lookup(std::optional<t_type>&& value) noexcept(std::is_nothrow_move_constructible_v<t_type>) :
        status_(unique_lookup_status::found),
        value_(std::move(value)) {}

    unique_lookup_status status_;
    std::optional<t_type> value_;
};

namespace detail {

template <typename derived_type, fixed_string default_token>
class json_token_base {
public:
    json_token_base(const json_token_base&) = delete;
    json_token_base& operator=(const json_token_base&) = delete;

    json_token_base(json_token_base&& other) noexcept :
        storage_(std::move(other.storage_)) {
        other.storage_.assign_borrowed(default_token.view());
    }

    json_token_base& operator=(json_token_base&& other) {
        if (this != &other) {
            storage_ = std::move(other.storage_);
            other.storage_.assign_borrowed(default_token.view());
        }
        return *this;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return storage_.view();
    }
    [[nodiscard]] std::string_view view() const&& = delete;

    [[nodiscard]] derived_type clone() const {
        return derived_type(storage_.rebind());
    }

    [[nodiscard]] derived_type take_owned() && {
        storage_.own_borrowed_in_place();
        return std::move(static_cast<derived_type&>(*this));
    }
    derived_type take_owned() & = delete;

    [[nodiscard]] std::string take_string() && {
        auto taken = std::move(storage_).take();
        storage_.assign_borrowed(default_token.view());
        return taken;
    }
    std::string take_string() & = delete;

    template <typename visitor_type>
    [[nodiscard]] bool for_each_field(visitor_type&& visitor) const&;
    template <typename visitor_type>
    bool for_each_field(visitor_type&&) const&& = delete;

    [[nodiscard]] friend bool operator==(const derived_type& left, const derived_type& right) noexcept {
        return left.view() == right.view();
    }

protected:
    friend struct model_value_factory;
    friend struct model_value_rebind_access;
    template <typename view_type>
    friend std::optional<view_type> parse_json_view_value(std::string_view&, std::size_t, model_string_storage, bool, json_parse_budget&);

    json_token_base(borrowed_model_token_tag, std::string_view body) noexcept :
        storage_(body) {}

    explicit json_token_base(model_text_storage&& storage) noexcept :
        storage_(std::move(storage)) {}

    template <typename model_type>
        requires(is_model<model_type> || is_tagged_variant<model_type>)
    explicit json_token_base(const model_type& value) {
        std::string output;
        output.reserve(json_size_hint_value(value) + 1);
        append_json_value(output, value);
        storage_.assign_owned(std::move(output));
    }

    template <typename body_type>
    [[nodiscard]] static model_parse_result<derived_type> parse_token(body_type&& body, model_parse_options options, bool require_object, const char* error_message) {
        if (!is_complete_json_value(body, options, require_object)) {
            return model_parse_result<derived_type>(model_parse_error(model_parse_error::code_type::invalid_format, error_message));
        }
        if constexpr (std::same_as<body_type, std::string>) {
            derived_type token;
            token.storage_.assign_owned(std::move(body));
            return model_parse_result<derived_type>(std::move(token));
        } else {
            return model_parse_result<derived_type>(derived_type(borrowed_model_token_tag{}, body));
        }
    }

    void assign_owned(std::string_view value) {
        storage_.assign_owned(value);
    }

    void own_for_model() {
        storage_.own_borrowed_in_place();
    }

    [[nodiscard]] derived_type rebind_for_model() const& {
        return clone();
    }

    [[nodiscard]] derived_type rebind_for_model() && {
        return std::move(*this).take_owned();
    }

    model_text_storage storage_;
};

}  // namespace detail

class json_value final : public detail::json_token_base<json_value, "null"> {
    using base_type = detail::json_token_base<json_value, "null">;

public:
    enum class kind_type : unsigned char {
        object,
        array,
        string,
        number,
        boolean,
        null,
    };

    json_value() noexcept :
        base_type(detail::borrowed_model_token_tag{}, "null") {}

    template <typename model_type>
        requires(detail::is_model<model_type> || detail::is_tagged_variant<model_type>)
    explicit json_value(const model_type& value) :
        base_type(value) {}

    [[nodiscard]] static model_parse_result<json_value> parse(borrowed_text body RUVIA_LIFETIMEBOUND, model_parse_options options = {});

    template <std::same_as<std::string> string_type>
    [[nodiscard]] static model_parse_result<json_value> parse(string_type&& body, model_parse_options options = {}) {
        return parse_token(std::move(body), options, false, "invalid JSON value");
    }

    json_value(const json_value&) = delete;
    json_value& operator=(const json_value&) = delete;
    json_value(json_value&&) noexcept = default;
    json_value& operator=(json_value&&) = default;

    [[nodiscard]] kind_type kind() const noexcept {
        auto input = view();
        skip_json_whitespace(input);
        if (input.empty()) {
            return kind_type::null;
        }
        switch (input.front()) {
            case '{':
                return kind_type::object;
            case '[':
                return kind_type::array;
            case '"':
                return kind_type::string;
            case 't':
            case 'f':
                return kind_type::boolean;
            case 'n':
                return kind_type::null;
            default:
                return kind_type::number;
        }
    }

    [[nodiscard]] bool is_object() const noexcept {
        return kind() == kind_type::object;
    }
    [[nodiscard]] bool is_array() const noexcept {
        return kind() == kind_type::array;
    }
    [[nodiscard]] bool is_string() const noexcept {
        return kind() == kind_type::string;
    }
    [[nodiscard]] bool is_number() const noexcept {
        return kind() == kind_type::number;
    }
    [[nodiscard]] bool is_boolean() const noexcept {
        return kind() == kind_type::boolean;
    }
    [[nodiscard]] bool is_null() const noexcept {
        return kind() == kind_type::null;
    }

    template <typename t_type>
        requires detail::is_model_field<t_type>
    [[nodiscard]] std::optional<t_type> get() const& RUVIA_LIFETIMEBOUND;
    template <typename t_type>
        requires detail::is_model_field<t_type>
    std::optional<t_type> get() const&& = delete;

    template <typename t_type>
        requires detail::is_model_field<t_type>
    [[nodiscard]] json_field_lookup<t_type> get(std::string_view field) const& RUVIA_LIFETIMEBOUND;
    template <typename t_type>
        requires detail::is_model_field<t_type>
    json_field_lookup<t_type> get(std::string_view field) const&& = delete;

    template <typename visitor_type>
    [[nodiscard]] bool for_each_element(visitor_type&& visitor) const&;
    template <typename visitor_type>
    bool for_each_element(visitor_type&&) const&& = delete;

    template <typename visitor_type>
        requires requires(visitor_type&& visitor, const json_value& value) { { std::forward<visitor_type>(visitor)(value) } -> std::same_as<void>; }
    [[nodiscard]] bool visit_pointer(std::string_view pointer, visitor_type&& visitor) const& {
        const auto selected = find_pointer(pointer);
        if (!selected) {
            return false;
        }
        const json_value token(detail::borrowed_model_token_tag{}, *selected);
        std::forward<visitor_type>(visitor)(token);
        return true;
    }
    template <typename visitor_type>
    bool visit_pointer(std::string_view, visitor_type&&) const&& = delete;

private:
    [[nodiscard]] std::optional<std::string_view> find_pointer(std::string_view pointer) const&;

    friend class json_object;
    template <typename, fixed_string>
    friend class detail::json_token_base;
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;
    template <typename view_type>
    friend std::optional<view_type> detail::parse_json_view_value(std::string_view&, std::size_t, detail::model_string_storage, bool, detail::json_parse_budget&);

    using base_type::base_type;
};

class json_object final : public detail::json_token_base<json_object, "{}"> {
    using base_type = detail::json_token_base<json_object, "{}">;

public:
    json_object() noexcept :
        base_type(detail::borrowed_model_token_tag{}, "{}") {}

    template <typename model_type>
        requires(detail::is_model<model_type> || detail::is_tagged_variant<model_type>)
    explicit json_object(const model_type& value) :
        base_type(value) {}

    [[nodiscard]] static model_parse_result<json_object> parse(borrowed_text body RUVIA_LIFETIMEBOUND, model_parse_options options = {});

    template <std::same_as<std::string> string_type>
    [[nodiscard]] static model_parse_result<json_object> parse(string_type&& body, model_parse_options options = {}) {
        return parse_token(std::move(body), options, true, "invalid JSON object");
    }

    json_object(const json_object&) = delete;
    json_object& operator=(const json_object&) = delete;
    json_object(json_object&&) noexcept = default;
    json_object& operator=(json_object&&) = default;

    template <typename t_type>
        requires detail::is_model_field<t_type>
    [[nodiscard]] json_field_lookup<t_type> get(std::string_view field) const& RUVIA_LIFETIMEBOUND;
    template <typename t_type>
        requires detail::is_model_field<t_type>
    json_field_lookup<t_type> get(std::string_view field) const&& = delete;

private:
    friend class json_value;
    template <typename, fixed_string>
    friend class detail::json_token_base;
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;
    template <typename view_type>
    friend std::optional<view_type> detail::parse_json_view_value(std::string_view&, std::size_t, detail::model_string_storage, bool, detail::json_parse_budget&);

    using base_type::base_type;
};

inline model_parse_result<json_value> json_value::parse(borrowed_text body, model_parse_options options) {
    return parse_token(body.view(), options, false, "invalid JSON value");
}

inline model_parse_result<json_object> json_object::parse(borrowed_text body, model_parse_options options) {
    return parse_token(body.view(), options, true, "invalid JSON object");
}

template <typename visitor_type>
[[nodiscard]] inline bool json_value::for_each_element(visitor_type&& visitor) const& {
    if (!is_array()) {
        return false;
    }
    auto input = view();
    if (!consume_json_array_elements(input, 0, [&visitor](std::string_view& element_input) {
        const auto start = element_input;
        if (!skip_json_value(element_input, 1)) {
            return false;
        }
        json_value value(detail::borrowed_model_token_tag{}, start.substr(0, start.size() - element_input.size()));
        return static_cast<bool>(visitor(value));
    })) {
        return false;
    }
    skip_json_whitespace(input);
    return input.empty();
}

namespace detail {

template <typename derived_type, fixed_string default_token>
template <typename visitor_type>
[[nodiscard]] inline bool json_token_base<derived_type, default_token>::for_each_field(visitor_type&& visitor) const& {
    if constexpr (std::is_same_v<derived_type, json_value>) {
        if (!static_cast<const json_value*>(this)->is_object()) {
            return false;
        }
    }
    return visit_json_object_fields(view(), [&](std::string_view key, std::string_view value_view) {
        skip_json_whitespace(value_view);
        json_value value(borrowed_model_token_tag{}, value_view);
        return static_cast<bool>(visitor(key, value));
    }) == json_object_visit_result::complete;
}

[[nodiscard]] inline unique_lookup<std::string_view> find_json_member(std::string_view object, std::string_view field) {
    unique_lookup<std::string_view> match;
    const auto visited = visit_json_object_fields(object, [&match, field](std::string_view key, std::string_view value) {
        if (key == field) {
            skip_json_whitespace(value);
            match.add_occurrence(value);
        }
        return match.status_ != unique_lookup_status::repeated;
    });
    if (visited == json_object_visit_result::invalid) {
        throw std::logic_error("json_object token is not a complete JSON object");
    }
    return match;
}

template <typename view_type>
[[nodiscard]] std::optional<view_type> parse_json_view_value(std::string_view& input, std::size_t depth, model_string_storage string_storage, bool require_object, json_parse_budget& budget) {
    auto remaining = input;
    skip_json_whitespace(remaining);
    if (require_object && (remaining.empty() || remaining.front() != '{')) {
        return std::nullopt;
    }
    const auto start = remaining;
    if (!budget.skip_value(remaining, depth)) {
        return std::nullopt;
    }
    const auto token = start.substr(0, start.size() - remaining.size());
    view_type value;
    if (string_storage == model_string_storage::owned) {
        if (!budget.consume_owned_text(token.size())) {
            return std::nullopt;
        }
        value.assign_owned(token);
    } else {
        value.storage_.assign_borrowed(token);
    }
    input = remaining;
    return value;
}

}  // namespace detail

inline std::optional<std::string_view> json_value::find_pointer(std::string_view pointer) const& {
    auto selected = view();
    if (pointer.empty()) {
        return selected;
    }
    skip_json_whitespace(selected);
    std::string decoded;
    while (!pointer.empty()) {
        if (pointer.front() != '/') {
            return std::nullopt;
        }
        pointer.remove_prefix(1);
        const auto separator = pointer.find('/');
        auto key = pointer.substr(0, separator);
        pointer = separator == std::string_view::npos ? std::string_view{} : pointer.substr(separator);

        if (selected.front() == '{') {
            if (const auto escape = key.find('~'); escape != std::string_view::npos) {
                decoded.assign(key.substr(0, escape));
                for (auto i = escape; i < key.size(); ++i) {
                    auto byte = key[i];
                    if (byte == '~') {
                        if (++i == key.size() || (key[i] != '0' && key[i] != '1')) {
                            return std::nullopt;
                        }
                        byte = key[i] == '0' ? '~' : '/';
                    }
                    decoded.push_back(byte);
                }
                key = decoded;
            }
            const auto match = detail::find_json_member(selected, key);
            if (match.status_ != unique_lookup_status::found) {
                return std::nullopt;
            }
            selected = match.value_;
        } else if (selected.front() == '[') {
            if (key.empty() || (key.size() > 1 && key.front() == '0')) {
                return std::nullopt;
            }
            const auto index = parse_integer<std::size_t>(key);
            if (!index) {
                return std::nullopt;
            }
            auto remaining = *index;
            std::optional<std::string_view> element;
            (void)consume_json_array_elements(selected, 0, [&](std::string_view& input) {
                const auto start = input;
                if (!skip_json_value(input, 1)) {
                    return false;
                }
                if (remaining == 0) {
                    element = start.substr(0, start.size() - input.size());
                    return false;
                }
                --remaining;
                return true;
            });
            if (!element) {
                return std::nullopt;
            }
            selected = *element;
        } else {
            return std::nullopt;
        }
    }
    return selected;
}

template <typename t_type>
    requires detail::is_model_json_value<t_type>
inline void append_json(std::string& output, const t_type& value) {
    if constexpr (!detail::is_model_scalar<t_type>) {
        output.reserve(output.size() + detail::json_size_hint_value(value) + 1);
    }
    detail::append_json_value(output, value);
}

template <typename t_type>
    requires detail::is_model_json_value<t_type>
[[nodiscard]] inline std::string to_json(const t_type& value) {
    std::string output;
    append_json(output, value);
    return output;
}

class validator final {
public:
    using issue_list_type = std::vector<validation_issue>;

    validator() noexcept = default;
    validator(const validator&) = default;

    validator& operator=(const validator& other) {
        if (this != &other) {
            auto copied = other.issues_;
            issues_.swap(copied);
        }
        return *this;
    }

    validator(validator&& other) noexcept :
        issues_(std::move(other.issues_)) {
        other.issues_.clear();
    }

    validator& operator=(validator&& other) noexcept {
        if (this != &other) {
            issues_ = std::move(other.issues_);
            other.issues_.clear();
        }
        return *this;
    }

    ~validator() = default;

    validator& add(std::string_view field, std::string_view code, std::string_view message) & RUVIA_LIFETIMEBOUND {
        issues_.push_back(validation_issue({.field_ = field, .code_ = code, .message_ = message}));
        return *this;
    }
    validator& add(std::string_view, std::string_view, std::string_view) && = delete;

    template <typename optional_type>
    validator& required(const optional_type& value, std::string_view field, std::string_view message = "is required") & RUVIA_LIFETIMEBOUND {
        if (!value) {
            add(field, "required", message);
        }
        return *this;
    }
    template <typename optional_type>
    validator& required(const optional_type&, std::string_view, std::string_view = "is required") && = delete;

    template <typename optional_type>
    validator& min_length(const optional_type& value, std::string_view field, std::size_t min_value, std::string_view message = "is too short") & RUVIA_LIFETIMEBOUND {
        if (value && detail::model::model_string(*value).size() < min_value) {
            add(field, "too_small", message);
        }
        return *this;
    }
    template <typename optional_type>
    validator& min_length(const optional_type&, std::string_view, std::size_t, std::string_view = "is too short") && = delete;

    template <typename optional_type>
    validator& max_length(const optional_type& value, std::string_view field, std::size_t max_value, std::string_view message = "is too long") & RUVIA_LIFETIMEBOUND {
        if (value && detail::model::model_string(*value).size() > max_value) {
            add(field, "too_big", message);
        }
        return *this;
    }
    template <typename optional_type>
    validator& max_length(const optional_type&, std::string_view, std::size_t, std::string_view = "is too long") && = delete;

    template <typename optional_type, typename min_type, typename max_type>
    validator& range(const optional_type& value, std::string_view field, min_type min_value, max_type max_value, std::string_view message = "is out of range") & RUVIA_LIFETIMEBOUND {
        if (value) {
            if (detail::validation_less(*value, min_value)) {
                add(field, "too_small", message);
            } else if (detail::validation_greater(*value, max_value)) {
                add(field, "too_big", message);
            }
        }
        return *this;
    }
    template <typename optional_type, typename min_type, typename max_type>
    validator& range(const optional_type&, std::string_view, min_type, max_type, std::string_view = "is out of range") && = delete;

    template <typename optional_type>
    validator& one_of(const optional_type& value, std::string_view field, std::initializer_list<std::string_view> allowed, std::string_view message = "is not allowed") & RUVIA_LIFETIMEBOUND {
        if (!value) {
            return *this;
        }
        const auto actual = detail::model::model_string(*value);
        for (const auto option : allowed) {
            if (actual == option) {
                return *this;
            }
        }
        add(field, "one_of", message);
        return *this;
    }
    template <typename optional_type>
    validator& one_of(const optional_type&, std::string_view, std::initializer_list<std::string_view>, std::string_view = "is not allowed") && = delete;

    [[nodiscard]] bool ok() const noexcept {
        return issues_.empty();
    }

    [[nodiscard]] const issue_list_type& issues() const& noexcept RUVIA_LIFETIMEBOUND {
        return issues_;
    }
    [[nodiscard]] const issue_list_type& issues() const&& = delete;

    [[nodiscard]] issue_list_type take_issues() && noexcept {
        auto issues = std::move(issues_);
        issues_.clear();
        return issues;
    }

    void prepend_issues(std::vector<validation_issue>&& earlier) {
        if (earlier.empty()) {
            return;
        }
        earlier.insert(earlier.end(), std::make_move_iterator(issues_.begin()), std::make_move_iterator(issues_.end()));
        issues_ = std::move(earlier);
    }

private:
    issue_list_type issues_;
};

template <typename model_type>
    requires detail::is_model<model_type>
void validate_model(const model_type& model, validator& out) {
    detail::model_validation_access::validate_model(model, out);
}

namespace detail {

template <typename t_type>
[[nodiscard]] model_parse_result<t_type> finish_model_parse(t_type&& value, std::vector<validation_issue>&& parse_issues = {}, bool force_invalid_format = false) {
    validator checks;
    bool structure_valid = !force_invalid_format && parse_issues.empty();
    if constexpr (model_validation_access::contains_model<t_type>()) {
        if (!model_validation_access::validate_value(value, checks)) {
            structure_valid = false;
        }
    }
    checks.prepend_issues(std::move(parse_issues));
    if (!checks.ok()) {
        const auto code = structure_valid ? model_parse_error::code_type::validation_failed : model_parse_error::code_type::invalid_format;
        return model_parse_result<t_type>(model_parse_error(code, code == model_parse_error::code_type::validation_failed ? "model validation failed" : "invalid model structure", std::move(checks).take_issues()));
    }
    return model_parse_result<t_type>(std::move(value));
}

template <typename t_type>
[[nodiscard]] model_parse_result<t_type> parse_json_with_storage(std::string_view body, model_string_storage storage_mode, model_parse_options options) {
    std::vector<validation_issue> issues;
    bool had_recorded_issues = false;
    auto parsed_value = parse_complete_json_value<t_type>(body, storage_mode, options, &issues, &had_recorded_issues);
    if (!parsed_value) {
        return model_parse_result<t_type>(model_parse_error(model_parse_error::code_type::invalid_format, "invalid JSON value", std::move(issues)));
    }
    return finish_model_parse(std::move(*parsed_value), std::move(issues), had_recorded_issues);
}

}  // namespace detail

template <typename t_type>
    requires detail::is_model<t_type>
[[nodiscard]] model_parse_result<t_type> parse_form(std::string_view body) {
    std::vector<validation_issue> issues;
    auto parsed = detail::model_parse_access::parse_form<t_type>(detail::form_body_input{.body_ = body, .string_storage_ = detail::model_string_storage::owned}, &issues);
    const bool had_elem_issues = !issues.empty();
    return detail::finish_model_parse(std::move(parsed), std::move(issues), had_elem_issues);
}

template <typename t_type>
    requires detail::is_model_json_value<t_type>
[[nodiscard]] model_parse_result<t_type> parse_json(std::string_view body, model_parse_options options = {}) {
    return detail::parse_json_with_storage<t_type>(body, detail::model_string_storage::owned, options);
}

// Zero-copy JSON parser: unescaped string fields and json_value/json_object
// slices borrow directly from `body` (`std::string&&` temporaries are rejected
// at compile time via `borrowed_text`).
template <typename t_type>
    requires detail::is_model_json_value<t_type>
[[nodiscard]] model_parse_result<t_type> parse_json_borrowed(borrowed_text body RUVIA_LIFETIMEBOUND, model_parse_options options = {}) {
    return detail::parse_json_with_storage<t_type>(body.view(), detail::model_string_storage::borrowed, options);
}

template <typename t_type, std::same_as<std::string> string_type>
    requires(detail::is_ruvia_json_value<t_type> || detail::is_ruvia_json_object<t_type>)
[[nodiscard]] model_parse_result<t_type> parse_json(string_type&& body, model_parse_options options = {}) {
    return t_type::parse(std::move(body), options);
}

template <typename model_type, typename token_type>
    requires((detail::is_model<model_type> || detail::is_tagged_variant<model_type>) && (detail::is_ruvia_json_value<token_type> || detail::is_ruvia_json_object<token_type>))
[[nodiscard]] model_parse_result<model_type> from_json(const token_type& value, model_parse_options options = {}) {
    return parse_json<model_type>(value.view(), options);
}

template <typename t_type>
    requires detail::is_model_field<t_type>
[[nodiscard]] inline std::optional<t_type> json_value::get() const& {
    auto parsed_value = detail::parse_json_document<t_type>(view(), detail::model_string_storage::borrowed);
    if (!parsed_value) {
        return std::nullopt;
    }
    if constexpr (detail::model_validation_access::contains_model<t_type>()) {
        if (!detail::model_validation_access::value_rules_valid(*parsed_value)) {
            return std::nullopt;
        }
    }
    return parsed_value;
}

template <typename t_type>
    requires detail::is_model_field<t_type>
[[nodiscard]] inline json_field_lookup<t_type> json_object::get(std::string_view field) const& {
    const auto match = detail::find_json_member(view(), field);
    if (match.status_ != unique_lookup_status::found) {
        return json_field_lookup<t_type>(match.status_);
    }
    auto parsed_value = detail::parse_json_document<t_type>(match.value_, detail::model_string_storage::borrowed);
    if (parsed_value) {
        if constexpr (detail::model_validation_access::contains_model<t_type>()) {
            if (!detail::model_validation_access::value_rules_valid(*parsed_value)) {
                parsed_value.reset();
            }
        }
    }
    return json_field_lookup<t_type>(std::move(parsed_value));
}

template <typename t_type>
    requires detail::is_model_field<t_type>
[[nodiscard]] inline json_field_lookup<t_type> json_value::get(std::string_view field) const& {
    if (!is_object()) {
        return json_field_lookup<t_type>(unique_lookup_status::absent);
    }
    const auto match = detail::find_json_member(view(), field);
    if (match.status_ != unique_lookup_status::found) {
        return json_field_lookup<t_type>(match.status_);
    }
    auto parsed_value = detail::parse_json_document<t_type>(match.value_, detail::model_string_storage::borrowed);
    if (parsed_value) {
        if constexpr (detail::model_validation_access::contains_model<t_type>()) {
            if (!detail::model_validation_access::value_rules_valid(*parsed_value)) {
                parsed_value.reset();
            }
        }
    }
    return json_field_lookup<t_type>(std::move(parsed_value));
}

// Validation rule macros.
#define RUVIA_MIN(value, message) ::ruvia::detail::model::min<value, ::ruvia::fixed_string{message}>
#define RUVIA_MAX(value, message) ::ruvia::detail::model::max<value, ::ruvia::fixed_string{message}>
#define RUVIA_GT(value, message) ::ruvia::detail::model::gt<value, ::ruvia::fixed_string{message}>
#define RUVIA_LT(value, message) ::ruvia::detail::model::lt<value, ::ruvia::fixed_string{message}>
#define RUVIA_MIN_BYTES(value, message) ::ruvia::detail::model::min_bytes<value, ::ruvia::fixed_string{message}>
#define RUVIA_MAX_BYTES(value, message) ::ruvia::detail::model::max_bytes<value, ::ruvia::fixed_string{message}>
#define RUVIA_MIN_CHARS(value, message) ::ruvia::detail::model::min_chars<value, ::ruvia::fixed_string{message}>
#define RUVIA_MAX_CHARS(value, message) ::ruvia::detail::model::max_chars<value, ::ruvia::fixed_string{message}>
#define RUVIA_NOT_EMPTY(message) ::ruvia::detail::model::not_empty<::ruvia::fixed_string{message}>
#define RUVIA_UUID(message) ::ruvia::detail::model::uuid_rule<::ruvia::fixed_string{message}>
#define RUVIA_ONE_OF(message, ...) ::ruvia::detail::model::one_of<::ruvia::fixed_string{message}, __VA_ARGS__>
#define RUVIA_EMAIL(message) ::ruvia::detail::model::email<::ruvia::fixed_string{message}>
#define RUVIA_PATTERN(pattern, message) ::ruvia::detail::model::pattern_rule<pattern, ::ruvia::fixed_string{message}>
#define RUVIA_CUSTOM(predicate, message) ::ruvia::detail::model::custom<predicate, ::ruvia::fixed_string{message}>
#define RUVIA_EACH(...) ::ruvia::detail::model::each_rule<__VA_ARGS__>

}  // namespace ruvia
