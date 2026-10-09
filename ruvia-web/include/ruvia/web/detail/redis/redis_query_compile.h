#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/model/traits.h"
#include "ruvia/web/detail/redis/redis_entity_key.h"
#include "ruvia/web/detail/redis/redis_predicate_storage.h"
#include "ruvia/web/detail/redis/redis_repository_config.h"
#include "ruvia/web/entity_rows.h"
#include "ruvia/web/redis/redis_find_options.h"

namespace ruvia::detail {

// A TAG value is kept in a shadow hash field. The leading x makes an empty
// value distinct from a missing field, and hexadecimal encoding keeps every
// byte outside RediSearch's TAG grammar.
[[nodiscard]] inline std::pmr::string encode_redis_tag(
    std::string_view value, std::pmr::memory_resource* resource) {
    resource = pmr_resource_or_default(resource);
    constexpr char digits[] = "0123456789abcdef";
    std::pmr::string encoded(resource);
    encoded.reserve(value.size() * 2U + 1U);
    encoded.push_back('x');
    for (const unsigned char byte : value) {
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0fU]);
    }
    return encoded;
}

namespace redis_query_detail {

constexpr long double max_exact_redis_number = 9007199254740991.0L;
constexpr std::string_view present_value = "1";

[[nodiscard]] inline bool is_safe_identifier(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (const unsigned char ch : value) {
        if (!(ch == '_' || (ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool is_valid_index_name(std::string_view value) noexcept {
    if (value.empty() || value.find('\0') != std::string_view::npos) {
        return false;
    }
    return true;
}

inline void append_argument(std::pmr::vector<std::pmr::string>& args,
    std::string_view value) {
    args.emplace_back(value.data(), value.size());
}

inline void append_unsigned(std::pmr::string& output, std::uint64_t value) {
    char buffer[3 * sizeof(std::uint64_t) + 3]{};
    const auto result_value = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (result_value.ec != std::errc{}) {
        throw std::invalid_argument("failed to format Redis Search integer");
    }
    output.append(buffer, result_value.ptr);
}

[[nodiscard]] inline std::pmr::string redis_index_name(
    std::string_view prefix, std::pmr::memory_resource* resource) {
    if (!is_valid_index_name(prefix)) {
        throw std::invalid_argument("invalid Redis entity prefix");
    }
    std::pmr::string result_value(prefix.data(), prefix.size(), resource);
    result_value += ":idx";
    return result_value;
}

[[nodiscard]] inline std::pmr::string redis_tag_shadow(
    std::string_view field, std::pmr::memory_resource* resource) {
    if (!is_safe_identifier(field)) {
        throw std::invalid_argument("invalid Redis entity column name");
    }
    std::pmr::string result_value("__ruvia_tag_", resource);
    result_value.append(field.data(), field.size());
    return result_value;
}

[[nodiscard]] inline std::pmr::string redis_sort_shadow(
    std::string_view field, std::pmr::memory_resource* resource) {
    if (!is_safe_identifier(field)) {
        throw std::invalid_argument("invalid Redis entity column name");
    }
    std::pmr::string result_value("__ruvia_sort_", resource);
    result_value.append(field.data(), field.size());
    return result_value;
}

[[nodiscard]] inline std::pmr::string redis_present_shadow(
    std::string_view field, std::pmr::memory_resource* resource) {
    if (!is_safe_identifier(field)) {
        throw std::invalid_argument("invalid Redis entity column name");
    }
    std::pmr::string result_value("__ruvia_present_", resource);
    result_value.append(field.data(), field.size());
    return result_value;
}

template <typename entity_type, typename function_type, std::size_t... index>
void for_each_entity_column_impl(function_type& function, std::index_sequence<index...>) {
    (function.template operator()<std::tuple_element_t<index,
            typename entity_type::columns_type>>(),
        ...);
}

template <typename entity_type, typename function_type>
void for_each_entity_column(function_type&& function) {
    for_each_entity_column_impl<entity_type>(function,
        std::make_index_sequence<std::tuple_size_v<typename entity_type::columns_type>>{});
}

struct column_info final {
    enum class value_kind_type : std::uint8_t {
        unsupported,
        text,
        bool_value,
        signed_value,
        unsigned_value,
        float32,
        float64,
    };
    value_kind_type value_kind_{value_kind_type::unsupported};
    bool found_{false};
    bool primary_key_{false};
};

template <typename t_type>
consteval column_info::value_kind_type column_value_kind() {
    using value_type = std::remove_cvref_t<t_type>;
    if constexpr (std::is_same_v<value_type, string> ||
                  std::is_same_v<value_type, std::pmr::string> ||
                  std::is_same_v<value_type, std::string>) {
        return column_info::value_kind_type::text;
    } else if constexpr (std::is_same_v<value_type, bool_value> || std::is_same_v<value_type, bool>) {
        return column_info::value_kind_type::bool_value;
    } else if constexpr (is_ruvia_scalar<value_type>) {
        return column_value_kind<model_scalar_value_t_type<value_type>>();
    } else if constexpr (std::is_integral_v<value_type>) {
        if constexpr (std::is_signed_v<value_type>) {
            return column_info::value_kind_type::signed_value;
        } else {
            return column_info::value_kind_type::unsigned_value;
        }
    } else if constexpr (std::is_same_v<value_type, float>) {
        return column_info::value_kind_type::float32;
    } else if constexpr (std::is_same_v<value_type, double>) {
        return column_info::value_kind_type::float64;
    } else {
        return column_info::value_kind_type::unsupported;
    }
}

template <typename entity_type>
[[nodiscard]] column_info find_column_info(std::string_view name) {
    column_info result;
    for_each_entity_column<entity_type>([&]<typename column_type> {
        if (column_type::name.view() == name) {
            result.found_ = true;
            result.value_kind_ = column_value_kind<typename column_type::value_type>();
            result.primary_key_ = column_type::options.primary_key_;
        }
    });
    if (!result.found_) {
        throw std::invalid_argument("unknown Redis query column");
    }
    if (!is_safe_identifier(name)) {
        throw std::invalid_argument("invalid Redis query column name");
    }
    return result;
}

template <typename entity_type>
void validate_entity_prefix(std::string_view table_value) {
    if (!table_value.empty() && table_value != entity_type::prefix()) {
        throw std::invalid_argument("Redis predicate belongs to another entity prefix");
    }
}

[[nodiscard]] inline bool is_numeric_column_kind(column_info::value_kind_type kind) noexcept {
    return kind == column_info::value_kind_type::signed_value ||
           kind == column_info::value_kind_type::unsigned_value ||
           kind == column_info::value_kind_type::float32 ||
           kind == column_info::value_kind_type::float64;
}

[[nodiscard]] inline bool is_integer_column_kind(column_info::value_kind_type kind) noexcept {
    return kind == column_info::value_kind_type::signed_value ||
           kind == column_info::value_kind_type::unsigned_value;
}

[[nodiscard]] inline bool is_text_column_kind(column_info::value_kind_type kind) noexcept {
    return kind == column_info::value_kind_type::text;
}

[[nodiscard]] inline std::pmr::string canonical_numeric(
    const redis_literal& value, column_info::value_kind_type column_type_value,
    std::pmr::memory_resource* resource) {
    resource = pmr_resource_or_default(resource);
    const auto type = redis_literal_access::type(value);
    if (type == redis_literal_kind::null) {
        throw std::invalid_argument("Redis numeric predicate cannot compare with NULL");
    }
    char buffer[128]{};
    std::to_chars_result result_value{};
    switch (type) {
        case redis_literal_kind::signed_integer: {
            const auto number = redis_literal_access::signed_value(value);
            if (std::fabs(static_cast<long double>(number)) > max_exact_redis_number) {
                throw std::invalid_argument("Redis numeric predicate exceeds exact range");
            }
            result_value = std::to_chars(std::begin(buffer), std::end(buffer), number);
            break;
        }
        case redis_literal_kind::unsigned_integer: {
            const auto number = redis_literal_access::unsigned_value(value);
            if (static_cast<long double>(number) > max_exact_redis_number) {
                throw std::invalid_argument("Redis numeric predicate exceeds exact range");
            }
            result_value = std::to_chars(std::begin(buffer), std::end(buffer), number);
            break;
        }
        case redis_literal_kind::real: {
            double number = redis_literal_access::real_value(value);
            if (!std::isfinite(number) ||
                std::fabs(static_cast<long double>(number)) > max_exact_redis_number) {
                throw std::invalid_argument("Redis numeric predicate is outside exact range");
            }
            if (column_type_value == column_info::value_kind_type::float32) {
                const auto real = static_cast<float>(number);
                if (!std::isfinite(real)) {
                    throw std::invalid_argument("Redis numeric predicate is not finite");
                }
                result_value = std::to_chars(std::begin(buffer), std::end(buffer), real,
                    std::chars_format::general, std::numeric_limits<float>::max_digits10);
            } else {
                result_value = std::to_chars(std::begin(buffer), std::end(buffer), number,
                    std::chars_format::general, std::numeric_limits<double>::max_digits10);
            }
            break;
        }
        case redis_literal_kind::boolean:
            buffer[0] = redis_literal_access::boolean_value(value) ? '1' : '0';
            result_value.ptr = buffer + 1;
            result_value.ec = std::errc{};
            break;
        default:
            throw std::invalid_argument("Redis numeric predicate requires a numeric value");
    }
    if (result_value.ec != std::errc{}) {
        throw std::invalid_argument("failed to format Redis numeric predicate");
    }
    return std::pmr::string(buffer, static_cast<std::size_t>(result_value.ptr - buffer), resource);
}

[[nodiscard]] inline std::pmr::string canonical_tag(
    const redis_literal& value, column_info::value_kind_type column_type_value,
    std::pmr::memory_resource* resource) {
    switch (redis_literal_access::type(value)) {
        case redis_literal_kind::string:
            if (!is_text_column_kind(column_type_value)) {
                throw std::invalid_argument("Redis TAG predicate value type does not match column");
            }
            return encode_redis_tag(redis_literal_access::text(value), resource);
        case redis_literal_kind::boolean:
            if (column_type_value != column_info::value_kind_type::bool_value) {
                throw std::invalid_argument("Redis TAG predicate value type does not match column");
            }
            return encode_redis_tag(redis_literal_access::boolean_value(value) ? "1" : "0", resource);
        case redis_literal_kind::signed_integer:
        case redis_literal_kind::unsigned_integer:
        case redis_literal_kind::real:
            if (!is_numeric_column_kind(column_type_value)) {
                throw std::invalid_argument("Redis TAG predicate value type does not match column");
            }
            return encode_redis_tag(canonical_numeric(value, column_type_value, resource), resource);
        default:
            throw std::invalid_argument("Redis TAG predicate requires a scalar value");
    }
}

[[nodiscard]] inline std::pmr::string canonical_text(
    const redis_literal& value, std::pmr::memory_resource* resource) {
    if (redis_literal_access::type(value) != redis_literal_kind::string) {
        throw std::invalid_argument("Redis TEXT predicate requires a string value");
    }
    return std::pmr::string(redis_literal_access::text(value), resource);
}

[[nodiscard]] inline std::pmr::string canonical_primary_key(
    const redis_literal& value, column_info::value_kind_type column_type_value,
    std::pmr::memory_resource* resource) {
    if (is_text_column_kind(column_type_value)) {
        return canonical_text(value, resource);
    }
    if (!is_integer_column_kind(column_type_value)) {
        throw std::invalid_argument("Redis primary key must be text or integer");
    }
    const auto type = redis_literal_access::type(value);
    char buffer[128]{};
    std::to_chars_result result_value{};
    switch (type) {
        case redis_literal_kind::signed_integer:
            result_value = std::to_chars(std::begin(buffer), std::end(buffer),
                redis_literal_access::signed_value(value));
            break;
        case redis_literal_kind::unsigned_integer:
            result_value = std::to_chars(std::begin(buffer), std::end(buffer),
                redis_literal_access::unsigned_value(value));
            break;
        case redis_literal_kind::real: {
            const double number = redis_literal_access::real_value(value);
            if (!std::isfinite(number) || std::trunc(number) != number ||
                std::fabs(static_cast<long double>(number)) > max_exact_redis_number) {
                throw std::invalid_argument("Redis integer primary key is invalid");
            }
            result_value = std::to_chars(std::begin(buffer), std::end(buffer),
                static_cast<std::int64_t>(number));
            break;
        }
        default:
            throw std::invalid_argument("Redis primary key requires a string or integer value");
    }
    if (result_value.ec != std::errc{}) {
        throw std::invalid_argument("failed to format Redis primary key");
    }
    return std::pmr::string(buffer, static_cast<std::size_t>(result_value.ptr - buffer), resource);
}

[[nodiscard]] inline redis_index_kind index_kind(
    const redis_mapping& mapping, std::string_view field) {
    const auto kind = mapping.index_kind(field);
    if (kind == redis_index_kind::none) {
        throw std::invalid_argument("Redis query column is not indexed");
    }
    return kind;
}

[[nodiscard]] inline std::pmr::string presence_term(
    std::string_view field, std::pmr::memory_resource* resource) {
    const auto shadow = redis_present_shadow(field, resource);
    std::pmr::string result(resource);
    result += '@';
    result.append(shadow.data(), shadow.size());
    result += ":{";
    // Presence markers are written as the raw value 1. They are indexed as a
    // TAG shadow field, but are not passed through the user TAG encoding.
    result.append(present_value.data(), present_value.size());
    result += '}';
    return result;
}

inline void append_presence(std::pmr::string& output, std::string_view field,
    std::pmr::memory_resource* resource) {
    const auto value = presence_term(field, resource);
    output.append(value.data(), value.size());
}

inline void append_tag_comparison(std::pmr::string& output, std::string_view field,
    const redis_literal& value, column_info::value_kind_type column_type_value, redis_binary_operator operation,
    std::pmr::memory_resource* resource) {
    const auto null = redis_literal_access::type(value) == redis_literal_kind::null;
    if (null) {
        if (operation == redis_binary_operator::equal) {
            output.push_back('-');
            append_presence(output, field, resource);
        } else if (operation == redis_binary_operator::not_equal) {
            append_presence(output, field, resource);
        } else {
            throw std::invalid_argument("unsupported NULL TAG comparison");
        }
        return;
    }
    const auto encoded = canonical_tag(value, column_type_value, resource);
    std::pmr::string term(resource);
    term += '@';
    term.append(field.data(), field.size());
    term += ":{";
    term.append(encoded.data(), encoded.size());
    term += '}';
    switch (operation) {
        case redis_binary_operator::equal:
            output.append(term.data(), term.size());
            return;
        case redis_binary_operator::not_equal:
            output.push_back('(');
            append_presence(output, field, resource);
            output += " -";
            output.append(term.data(), term.size());
            output.push_back(')');
            return;
        default:
            throw std::invalid_argument("Redis TAG indexes only support equality predicates");
    }
}

inline void append_numeric_comparison(std::pmr::string& output, std::string_view field,
    const redis_literal& value, column_info::value_kind_type column_type_value, redis_binary_operator operation,
    std::pmr::memory_resource* resource) {
    const auto null = redis_literal_access::type(value) == redis_literal_kind::null;
    if (null) {
        if (operation == redis_binary_operator::equal) {
            output.push_back('-');
            append_presence(output, field, resource);
        } else if (operation == redis_binary_operator::not_equal) {
            append_presence(output, field, resource);
        } else {
            throw std::invalid_argument("unsupported NULL NUMERIC comparison");
        }
        return;
    }
    const auto number = canonical_numeric(value, column_type_value, resource);
    const bool not_equal = operation == redis_binary_operator::not_equal;
    if (not_equal) {
        output.push_back('(');
    }
    output.push_back('@');
    output.append(field.data(), field.size());
    output += ":[";
    switch (operation) {
        case redis_binary_operator::equal:
            output.append(number.data(), number.size());
            output.push_back(' ');
            output.append(number.data(), number.size());
            break;
        case redis_binary_operator::not_equal:
            output += "-inf +inf] -@";
            output.append(field.data(), field.size());
            output += ":[";
            output.append(number.data(), number.size());
            output.push_back(' ');
            output.append(number.data(), number.size());
            output.push_back(']');
            output.push_back(')');
            return;
        case redis_binary_operator::less:
            output += "-inf (";
            output.append(number.data(), number.size());
            break;
        case redis_binary_operator::less_equal:
            output += "-inf ";
            output.append(number.data(), number.size());
            break;
        case redis_binary_operator::greater:
            output.push_back('(');
            output.append(number.data(), number.size());
            output += " +inf";
            break;
        case redis_binary_operator::greater_equal:
            output.append(number.data(), number.size());
            output += " +inf";
            break;
        default:
            throw std::invalid_argument("unsupported Redis NUMERIC comparison");
    }
    output.push_back(']');
}

template <typename entity_type>
void append_expression(std::pmr::string& output, redis_expression expression,
    const redis_mapping& mapping, std::pmr::memory_resource* resource,
    std::size_t depth = 0U);

template <typename entity_type>
void append_indexed_comparison(std::pmr::string& output, std::string_view field,
    const redis_literal& value, redis_binary_operator operation, const redis_mapping& mapping,
    std::pmr::memory_resource* resource) {
    const auto info = find_column_info<entity_type>(field);
    switch (index_kind(mapping, field)) {
        case redis_index_kind::tag:
            append_tag_comparison(output, field, value, info.value_kind_, operation, resource);
            return;
        case redis_index_kind::numeric:
            append_numeric_comparison(output, field, value, info.value_kind_, operation, resource);
            return;
        case redis_index_kind::text:
            if (redis_literal_access::type(value) == redis_literal_kind::null &&
                (operation == redis_binary_operator::equal || operation == redis_binary_operator::not_equal)) {
                if (operation == redis_binary_operator::equal) {
                    output.push_back('-');
                    append_presence(output, field, resource);
                } else {
                    append_presence(output, field, resource);
                }
                return;
            }
            throw std::invalid_argument("Redis TEXT predicates are not exposed by redis_predicate");
        default:
            throw std::invalid_argument("invalid Redis query index kind");
    }
}

template <typename entity_type>
void append_in_list(std::pmr::string& output, std::string_view field,
    redis_expression list, redis_binary_operator operation, const redis_mapping& mapping,
    std::pmr::memory_resource* resource) {
    const auto list_info = redis_expression_access::inspect(list);
    if (list_info.kind_ != redis_expression_inspection::kind_type::list) {
        throw std::invalid_argument("Redis IN requires a literal list");
    }
    const auto count = redis_expression_access::operand_count(list);
    const bool negated = operation == redis_binary_operator::not_in;
    if (count == 0U) {
        if (negated) {
            output.push_back('*');
        } else {
            const auto presence = redis_present_shadow(field, resource);
            output += '@';
            output.append(presence.data(), presence.size());
            output += ":{x00}";
        }
        return;
    }
    if (negated) {
        output.push_back('(');
        append_presence(output, field, resource);
        output += " -(";
    } else {
        output.push_back('(');
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0U) {
            output.push_back('|');
        }
        const auto item = redis_expression_access::operand(list, index);
        const auto item_info = redis_expression_access::inspect(item);
        if (item_info.kind_ != redis_expression_inspection::kind_type::literal || item_info.value_ == nullptr ||
            redis_literal_access::type(*item_info.value_) == redis_literal_kind::null) {
            throw std::invalid_argument("Redis IN list requires non-null literal values");
        }
        const auto kind = index_kind(mapping, field);
        const auto info = find_column_info<entity_type>(field);
        if (kind == redis_index_kind::tag) {
            const auto encoded = canonical_tag(*item_info.value_, info.value_kind_, resource);
            output += '@';
            output.append(field.data(), field.size());
            output += ":{";
            output.append(encoded.data(), encoded.size());
            output.push_back('}');
        } else if (kind == redis_index_kind::numeric) {
            const auto number = canonical_numeric(*item_info.value_, info.value_kind_, resource);
            output += '@';
            output.append(field.data(), field.size());
            output += ":[";
            output.append(number.data(), number.size());
            output.push_back(' ');
            output.append(number.data(), number.size());
            output.push_back(']');
        } else {
            throw std::invalid_argument("Redis TEXT predicates are not supported");
        }
    }
    output += ")";
    if (negated) {
        output.push_back(')');
    }
}

template <typename entity_type>
void append_between(std::pmr::string& output, redis_expression expression,
    const redis_mapping& mapping, std::pmr::memory_resource* resource) {
    const auto inspection = redis_expression_access::inspect(expression);
    if (inspection.kind_ != redis_expression_inspection::kind_type::between ||
        redis_expression_access::operand_count(expression) != 3U) {
        throw std::invalid_argument("invalid Redis BETWEEN expression");
    }
    const auto column = redis_expression_access::operand(expression, 0);
    const auto lower = redis_expression_access::operand(expression, 1);
    const auto upper = redis_expression_access::operand(expression, 2);
    const auto column_info_value = redis_expression_access::inspect(column);
    const auto lower_info = redis_expression_access::inspect(lower);
    const auto upper_info = redis_expression_access::inspect(upper);
    if (column_info_value.kind_ != redis_expression_inspection::kind_type::field ||
        lower_info.kind_ != redis_expression_inspection::kind_type::literal || lower_info.value_ == nullptr ||
        upper_info.kind_ != redis_expression_inspection::kind_type::literal || upper_info.value_ == nullptr) {
        throw std::invalid_argument("Redis BETWEEN requires a column and literal bounds");
    }
    validate_entity_prefix<entity_type>(column_info_value.entity_prefix_);
    const auto field = column_info_value.field_;
    const auto info = find_column_info<entity_type>(field);
    if (index_kind(mapping, field) != redis_index_kind::numeric ||
        redis_literal_access::type(*lower_info.value_) == redis_literal_kind::null ||
        redis_literal_access::type(*upper_info.value_) == redis_literal_kind::null) {
        throw std::invalid_argument("Redis BETWEEN requires a NUMERIC indexed column");
    }
    const auto low = canonical_numeric(*lower_info.value_, info.value_kind_, resource);
    const auto high = canonical_numeric(*upper_info.value_, info.value_kind_, resource);
    std::pmr::string range(resource);
    range += '@';
    range.append(field.data(), field.size());
    range += ":[";
    range.append(low.data(), low.size());
    range.push_back(' ');
    range.append(high.data(), high.size());
    range.push_back(']');
    output.append(range.data(), range.size());
}

template <typename entity_type>
void append_expression(std::pmr::string& output, redis_expression expression,
    const redis_mapping& mapping, std::pmr::memory_resource* resource,
    std::size_t depth) {
    constexpr std::size_t max_depth = 64U;
    if (depth > max_depth) {
        throw std::invalid_argument("Redis predicate nesting is too deep");
    }
    const auto inspection = redis_expression_access::inspect(expression);
    using kind_type = redis_expression_inspection::kind_type;
    if (inspection.kind_ == kind_type::between) {
        append_between<entity_type>(output, expression, mapping, resource);
        return;
    }
    if (inspection.kind_ == kind_type::binary) {
        if (inspection.binary_ == redis_binary_operator::logical_and ||
            inspection.binary_ == redis_binary_operator::logical_or) {
            if (redis_expression_access::operand_count(expression) != 2U) {
                throw std::invalid_argument("invalid Redis logical expression");
            }
            output.push_back('(');
            append_expression<entity_type>(output, redis_expression_access::operand(expression, 0), mapping, resource, depth + 1U);
            output.push_back(inspection.binary_ == redis_binary_operator::logical_and ? ' ' : '|');
            append_expression<entity_type>(output, redis_expression_access::operand(expression, 1), mapping, resource, depth + 1U);
            output.push_back(')');
            return;
        }
        if (inspection.binary_ == redis_binary_operator::in ||
            inspection.binary_ == redis_binary_operator::not_in) {
            if (redis_expression_access::operand_count(expression) != 2U) {
                throw std::invalid_argument("invalid Redis IN expression");
            }
            const auto left = redis_expression_access::inspect(redis_expression_access::operand(expression, 0));
            if (left.kind_ != kind_type::field) {
                throw std::invalid_argument("Redis IN requires a column");
            }
            validate_entity_prefix<entity_type>(left.entity_prefix_);
            (void)find_column_info<entity_type>(left.field_);
            append_in_list<entity_type>(output, left.field_,
                redis_expression_access::operand(expression, 1), inspection.binary_, mapping, resource);
            return;
        }
        if (inspection.binary_ != redis_binary_operator::equal &&
            inspection.binary_ != redis_binary_operator::not_equal &&
            inspection.binary_ != redis_binary_operator::less &&
            inspection.binary_ != redis_binary_operator::less_equal &&
            inspection.binary_ != redis_binary_operator::greater &&
            inspection.binary_ != redis_binary_operator::greater_equal) {
            throw std::invalid_argument("unsupported Redis predicate operator");
        }
        if (redis_expression_access::operand_count(expression) != 2U) {
            throw std::invalid_argument("invalid Redis comparison expression");
        }
        const auto left_expression = redis_expression_access::operand(expression, 0);
        const auto right_expression = redis_expression_access::operand(expression, 1);
        const auto left = redis_expression_access::inspect(left_expression);
        const auto right = redis_expression_access::inspect(right_expression);
        redis_expression_inspection column{};
        const redis_literal* value = nullptr;
        auto operation = inspection.binary_;
        if (left.kind_ == kind_type::field && right.kind_ == kind_type::literal) {
            column = left;
            value = right.value_;
        } else if (right.kind_ == kind_type::field && left.kind_ == kind_type::literal) {
            column = right;
            value = left.value_;
            switch (operation) {
                case redis_binary_operator::less:
                    operation = redis_binary_operator::greater;
                    break;
                case redis_binary_operator::less_equal:
                    operation = redis_binary_operator::greater_equal;
                    break;
                case redis_binary_operator::greater:
                    operation = redis_binary_operator::less;
                    break;
                case redis_binary_operator::greater_equal:
                    operation = redis_binary_operator::less_equal;
                    break;
                default:
                    break;
            }
        } else {
            throw std::invalid_argument("Redis comparison requires one column and one literal");
        }
        if (value == nullptr) {
            throw std::invalid_argument("Redis comparison has no literal value");
        }
        validate_entity_prefix<entity_type>(column.entity_prefix_);
        append_indexed_comparison<entity_type>(output, column.field_, *value,
            operation, mapping, resource);
        return;
    }
    throw std::invalid_argument("unsupported Redis expression");
}

template <typename entity_type>
[[nodiscard]] std::pmr::string sort_field(std::string_view field,
    const redis_mapping& mapping, std::pmr::memory_resource* resource) {
    (void)find_column_info<entity_type>(field);
    if (!mapping.sortable(field)) {
        throw std::invalid_argument("Redis order column is not sortable");
    }
    if (mapping.index_kind(field) == redis_index_kind::tag) {
        return redis_sort_shadow(field, resource);
    }
    if (mapping.index_kind(field) == redis_index_kind::none) {
        throw std::invalid_argument("Redis order column is not indexed");
    }
    return std::pmr::string(field.data(), field.size(), resource);
}

}  // namespace redis_query_detail

template <typename entity_type>
[[nodiscard]] std::pmr::vector<std::pmr::string> compile_redis_find(
    const redis_find_options& options, const redis_mapping& mapping,
    std::pmr::memory_resource* resource, bool count_only = false,
    std::optional<std::uint64_t> take_override = {}) {
    resource = pmr_resource_or_default(resource);
    std::pmr::vector<std::pmr::string> args(resource);
    const auto index = redis_query_detail::redis_index_name(mapping.prefix_, resource);
    redis_query_detail::append_argument(args, "FT.SEARCH");
    redis_query_detail::append_argument(args, index);
    std::pmr::string query(resource);
    const auto root = redis_predicate_access::root(options.where_);
    if (root.empty()) {
        query = "*";
    } else {
        redis_query_detail::append_expression<entity_type>(query, root, mapping, resource);
    }
    args.emplace_back(std::move(query));
    redis_query_detail::append_argument(args, "LIMIT");
    if (count_only) {
        redis_query_detail::append_argument(args, "0");
        redis_query_detail::append_argument(args, "0");
    } else {
        constexpr auto max_redis_integer = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const auto skip = options.skip_.value_or(0U);
        const auto take = take_override.value_or(options.take_.value_or(100U));
        if (skip > max_redis_integer || take > max_redis_integer) {
            throw std::invalid_argument("Redis Search LIMIT exceeds signed 64-bit range");
        }
        std::pmr::string encoded_skip(resource), encoded_take(resource);
        redis_query_detail::append_unsigned(encoded_skip, skip);
        redis_query_detail::append_unsigned(encoded_take, take);
        args.emplace_back(std::move(encoded_skip));
        args.emplace_back(std::move(encoded_take));
    }
    if (!count_only && !options.order_.empty()) {
        if (options.order_.size() != 1U) {
            throw std::invalid_argument("Redis Search supports one SORTBY column per query");
        }
        const auto& order = options.order_.front();
        const auto field = redis_query_detail::sort_field<entity_type>(order.field_, mapping, resource);
        redis_query_detail::append_argument(args, "SORTBY");
        redis_query_detail::append_argument(args, field);
        switch (order.direction_) {
            case redis_order_direction::ascending:
                redis_query_detail::append_argument(args, "ASC");
                break;
            case redis_order_direction::descending:
                redis_query_detail::append_argument(args, "DESC");
                break;
            default:
                throw std::invalid_argument("invalid Redis order direction");
        }
    }
    redis_query_detail::append_argument(args, "DIALECT");
    redis_query_detail::append_argument(args, "2");
    return args;
}

template <typename entity_type>
[[nodiscard]] std::pmr::vector<std::pmr::string> compile_redis_index(
    const redis_mapping& mapping, std::pmr::memory_resource* resource) {
    resource = pmr_resource_or_default(resource);
    std::pmr::vector<std::pmr::string> args(resource);
    const auto index = redis_query_detail::redis_index_name(mapping.prefix_, resource);
    const auto key_prefix = redis_entity_storage_prefix(mapping.prefix_, resource);
    redis_query_detail::append_argument(args, "FT.CREATE");
    redis_query_detail::append_argument(args, index);
    redis_query_detail::append_argument(args, "ON");
    redis_query_detail::append_argument(args, "HASH");
    redis_query_detail::append_argument(args, "PREFIX");
    redis_query_detail::append_argument(args, "1");
    redis_query_detail::append_argument(args, key_prefix);
    redis_query_detail::append_argument(args, "SCHEMA");
    if (mapping.indexes_.empty()) {
        throw std::invalid_argument("Redis repository requires at least one index column");
    }
    std::size_t schema_count = 0U;
    for (const auto& definition : mapping.indexes_) {
        const auto field = std::string_view(definition.field_);
        (void)redis_query_detail::find_column_info<entity_type>(field);
        if (!redis_query_detail::is_safe_identifier(field)) {
            throw std::invalid_argument("invalid Redis index column name");
        }
        if (definition.kind_ == redis_index_kind::none ||
            mapping.index_kind(field) != definition.kind_ ||
            mapping.sortable(field) != definition.sortable_) {
            throw std::invalid_argument("Redis mapping index metadata is inconsistent");
        }
        if (field.starts_with("__ruvia_")) {
            throw std::invalid_argument("Redis index column uses reserved __ruvia_ namespace");
        }
        for (const auto& other : mapping.indexes_) {
            if (&other != &definition && other.field_ == definition.field_) {
                throw std::invalid_argument("duplicate Redis index column");
            }
        }
        const auto value_kind = redis_query_detail::find_column_info<entity_type>(field).value_kind_;
        if ((definition.kind_ == redis_index_kind::tag &&
                value_kind != redis_query_detail::column_info::value_kind_type::bool_value &&
                !redis_query_detail::is_text_column_kind(value_kind)) ||
            (definition.kind_ == redis_index_kind::text &&
                !redis_query_detail::is_text_column_kind(value_kind)) ||
            (definition.kind_ == redis_index_kind::numeric &&
                !redis_query_detail::is_numeric_column_kind(value_kind))) {
            throw std::invalid_argument("Redis index kind does not match entity column type");
        }
        const auto presence = redis_query_detail::redis_present_shadow(field, resource);
        switch (definition.kind_) {
            case redis_index_kind::tag: {
                const auto shadow = redis_query_detail::redis_tag_shadow(field, resource);
                redis_query_detail::append_argument(args, shadow);
                redis_query_detail::append_argument(args, "AS");
                redis_query_detail::append_argument(args, field);
                redis_query_detail::append_argument(args, "TAG");
                redis_query_detail::append_argument(args, "CASESENSITIVE");
                ++schema_count;
                break;
            }
            case redis_index_kind::text:
                redis_query_detail::append_argument(args, field);
                redis_query_detail::append_argument(args, "TEXT");
                if (definition.sortable_) {
                    redis_query_detail::append_argument(args, "SORTABLE");
                }
                ++schema_count;
                break;
            case redis_index_kind::numeric:
                redis_query_detail::append_argument(args, field);
                redis_query_detail::append_argument(args, "NUMERIC");
                if (definition.sortable_) {
                    redis_query_detail::append_argument(args, "SORTABLE");
                }
                ++schema_count;
                break;
            default:
                throw std::invalid_argument("invalid Redis index kind");
        }
        redis_query_detail::append_argument(args, presence);
        redis_query_detail::append_argument(args, "AS");
        redis_query_detail::append_argument(args, presence);
        redis_query_detail::append_argument(args, "TAG");
        redis_query_detail::append_argument(args, "CASESENSITIVE");
        ++schema_count;
        if (definition.kind_ == redis_index_kind::tag && definition.sortable_) {
            const auto sort_shadow = redis_query_detail::redis_sort_shadow(field, resource);
            redis_query_detail::append_argument(args, field);
            redis_query_detail::append_argument(args, "AS");
            redis_query_detail::append_argument(args, sort_shadow);
            redis_query_detail::append_argument(args, "TAG");
            redis_query_detail::append_argument(args, "SORTABLE");
            redis_query_detail::append_argument(args, "NOINDEX");
            ++schema_count;
        }
    }
    if (schema_count == 0U) {
        throw std::invalid_argument("Redis repository requires an indexed column");
    }
    return args;
}

template <typename entity_type>
[[nodiscard]] std::optional<std::pmr::string> redis_primary_key(
    const redis_predicate& predicate, std::pmr::memory_resource* resource) {
    resource = pmr_resource_or_default(resource);
    const auto root = redis_predicate_access::root(predicate);
    if (root.empty()) {
        return std::nullopt;
    }
    const auto root_info = redis_expression_access::inspect(root);
    if (root_info.kind_ != redis_expression_inspection::kind_type::binary ||
        root_info.binary_ != redis_binary_operator::equal ||
        redis_expression_access::operand_count(root) != 2U) {
        return std::nullopt;
    }
    const auto left_expression = redis_expression_access::operand(root, 0);
    const auto right_expression = redis_expression_access::operand(root, 1);
    const auto left = redis_expression_access::inspect(left_expression);
    const auto right = redis_expression_access::inspect(right_expression);
    redis_expression_inspection column{};
    const redis_literal* value = nullptr;
    if (left.kind_ == redis_expression_inspection::kind_type::field && right.kind_ == redis_expression_inspection::kind_type::literal) {
        column = left;
        value = right.value_;
    } else if (right.kind_ == redis_expression_inspection::kind_type::field && left.kind_ == redis_expression_inspection::kind_type::literal) {
        column = right;
        value = left.value_;
    } else {
        return std::nullopt;
    }
    redis_query_detail::validate_entity_prefix<entity_type>(column.entity_prefix_);
    const auto info = redis_query_detail::find_column_info<entity_type>(column.field_);
    if (!info.primary_key_) {
        return std::nullopt;
    }
    if (value == nullptr || redis_literal_access::type(*value) == redis_literal_kind::null) {
        throw std::invalid_argument("Redis primary key cannot be NULL");
    }
    if (!redis_query_detail::is_text_column_kind(info.value_kind_) &&
        !redis_query_detail::is_integer_column_kind(info.value_kind_)) {
        throw std::invalid_argument("Redis primary key must be text or integer");
    }
    auto result_value = redis_query_detail::canonical_primary_key(*value, info.value_kind_, resource);
    if (result_value.empty()) {
        throw std::invalid_argument("Redis primary key cannot be empty");
    }
    return result_value;
}

}  // namespace ruvia::detail
