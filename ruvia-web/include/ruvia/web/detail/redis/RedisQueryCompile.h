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

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/EntityRows.h"
#include "ruvia/web/detail/model/Traits.h"
#include "ruvia/web/detail/redis/RedisEntityKey.h"
#include "ruvia/web/detail/redis/RedisPredicateStorage.h"
#include "ruvia/web/detail/redis/RedisRepositoryConfig.h"
#include "ruvia/web/redis/RedisFindOptions.h"

namespace ruvia::detail {

// A TAG value is kept in a shadow hash field. The leading x makes an empty
// value distinct from a missing field, and hexadecimal encoding keeps every
// byte outside RediSearch's TAG grammar.
[[nodiscard]] inline std::pmr::string encodeRedisTag(
    std::string_view value, std::pmr::memory_resource* resource) {
    resource = pmrResourceOrDefault(resource);
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

constexpr long double kMaxExactRedisNumber = 9007199254740991.0L;
constexpr std::string_view kPresentValue = "1";

[[nodiscard]] inline bool isSafeIdentifier(std::string_view value) noexcept {
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

[[nodiscard]] inline bool isValidIndexName(std::string_view value) noexcept {
    if (value.empty() || value.find('\0') != std::string_view::npos) {
        return false;
    }
    return true;
}

inline void appendArgument(std::pmr::vector<std::pmr::string>& args,
    std::string_view value) {
    args.emplace_back(value.data(), value.size());
}

inline void appendUnsigned(std::pmr::string& output, std::uint64_t value) {
    char buffer[3 * sizeof(std::uint64_t) + 3]{};
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (result.ec != std::errc{}) {
        throw std::invalid_argument("failed to format Redis Search integer");
    }
    output.append(buffer, result.ptr);
}

[[nodiscard]] inline std::pmr::string redisIndexName(
    std::string_view prefix, std::pmr::memory_resource* resource) {
    if (!isValidIndexName(prefix)) {
        throw std::invalid_argument("invalid Redis entity prefix");
    }
    std::pmr::string result(prefix.data(), prefix.size(), resource);
    result += ":idx";
    return result;
}

[[nodiscard]] inline std::pmr::string redisTagShadow(
    std::string_view field, std::pmr::memory_resource* resource) {
    if (!isSafeIdentifier(field)) {
        throw std::invalid_argument("invalid Redis entity column name");
    }
    std::pmr::string result("__ruvia_tag_", resource);
    result.append(field.data(), field.size());
    return result;
}

[[nodiscard]] inline std::pmr::string redisSortShadow(
    std::string_view field, std::pmr::memory_resource* resource) {
    if (!isSafeIdentifier(field)) {
        throw std::invalid_argument("invalid Redis entity column name");
    }
    std::pmr::string result("__ruvia_sort_", resource);
    result.append(field.data(), field.size());
    return result;
}

[[nodiscard]] inline std::pmr::string redisPresentShadow(
    std::string_view field, std::pmr::memory_resource* resource) {
    if (!isSafeIdentifier(field)) {
        throw std::invalid_argument("invalid Redis entity column name");
    }
    std::pmr::string result("__ruvia_present_", resource);
    result.append(field.data(), field.size());
    return result;
}

template <typename Entity, typename Function, std::size_t... Index>
void forEachEntityColumnImpl(Function& function, std::index_sequence<Index...>) {
    (function.template operator()<std::tuple_element_t<Index,
            typename Entity::Columns>>(),
        ...);
}

template <typename Entity, typename Function>
void forEachEntityColumn(Function&& function) {
    forEachEntityColumnImpl<Entity>(function,
        std::make_index_sequence<std::tuple_size_v<typename Entity::Columns>>{});
}

struct ColumnInfo final {
    enum class ValueKind : std::uint8_t {
        kUnsupported,
        kText,
        kBool,
        kSigned,
        kUnsigned,
        kFloat32,
        kFloat64,
    };
    ValueKind valueKind{ValueKind::kUnsupported};
    bool found{false};
    bool primaryKey{false};
};

template <typename T>
consteval ColumnInfo::ValueKind columnValueKind() {
    using Value = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<Value, String> ||
                  std::is_same_v<Value, std::pmr::string> ||
                  std::is_same_v<Value, std::string>) {
        return ColumnInfo::ValueKind::kText;
    } else if constexpr (std::is_same_v<Value, Bool> || std::is_same_v<Value, bool>) {
        return ColumnInfo::ValueKind::kBool;
    } else if constexpr (isRuviaScalar<Value>) {
        return columnValueKind<ModelScalarValueT<Value>>();
    } else if constexpr (std::is_integral_v<Value>) {
        if constexpr (std::is_signed_v<Value>) {
            return ColumnInfo::ValueKind::kSigned;
        } else {
            return ColumnInfo::ValueKind::kUnsigned;
        }
    } else if constexpr (std::is_same_v<Value, float>) {
        return ColumnInfo::ValueKind::kFloat32;
    } else if constexpr (std::is_same_v<Value, double>) {
        return ColumnInfo::ValueKind::kFloat64;
    } else {
        return ColumnInfo::ValueKind::kUnsupported;
    }
}

template <typename Entity>
[[nodiscard]] ColumnInfo columnInfo(std::string_view name) {
    ColumnInfo result;
    forEachEntityColumn<Entity>([&]<typename Column> {
        if (Column::name.view() == name) {
            result.found = true;
            result.valueKind = columnValueKind<typename Column::value_type>();
            result.primaryKey = Column::options.primaryKey;
        }
    });
    if (!result.found) {
        throw std::invalid_argument("unknown Redis query column");
    }
    if (!isSafeIdentifier(name)) {
        throw std::invalid_argument("invalid Redis query column name");
    }
    return result;
}

template <typename Entity>
void validate_entity_prefix(std::string_view table) {
    if (!table.empty() && table != Entity::prefix()) {
        throw std::invalid_argument("Redis predicate belongs to another entity prefix");
    }
}

[[nodiscard]] inline bool isNumericColumnKind(ColumnInfo::ValueKind kind) noexcept {
    return kind == ColumnInfo::ValueKind::kSigned ||
           kind == ColumnInfo::ValueKind::kUnsigned ||
           kind == ColumnInfo::ValueKind::kFloat32 ||
           kind == ColumnInfo::ValueKind::kFloat64;
}

[[nodiscard]] inline bool isIntegerColumnKind(ColumnInfo::ValueKind kind) noexcept {
    return kind == ColumnInfo::ValueKind::kSigned ||
           kind == ColumnInfo::ValueKind::kUnsigned;
}

[[nodiscard]] inline bool isTextColumnKind(ColumnInfo::ValueKind kind) noexcept {
    return kind == ColumnInfo::ValueKind::kText;
}

[[nodiscard]] inline std::pmr::string canonicalNumeric(
    const redis_literal& value, ColumnInfo::ValueKind columnType,
    std::pmr::memory_resource* resource) {
    resource = pmrResourceOrDefault(resource);
    const auto type = redis_literal_access::type(value);
    if (type == redis_literal_kind::null) {
        throw std::invalid_argument("Redis numeric predicate cannot compare with NULL");
    }
    char buffer[128]{};
    std::to_chars_result result{};
    switch (type) {
        case redis_literal_kind::signed_integer: {
            const auto number = redis_literal_access::signed_value(value);
            if (std::fabs(static_cast<long double>(number)) > kMaxExactRedisNumber) {
                throw std::invalid_argument("Redis numeric predicate exceeds exact range");
            }
            result = std::to_chars(std::begin(buffer), std::end(buffer), number);
            break;
        }
        case redis_literal_kind::unsigned_integer: {
            const auto number = redis_literal_access::unsigned_value(value);
            if (static_cast<long double>(number) > kMaxExactRedisNumber) {
                throw std::invalid_argument("Redis numeric predicate exceeds exact range");
            }
            result = std::to_chars(std::begin(buffer), std::end(buffer), number);
            break;
        }
        case redis_literal_kind::real: {
            double number = redis_literal_access::real_value(value);
            if (!std::isfinite(number) ||
                std::fabs(static_cast<long double>(number)) > kMaxExactRedisNumber) {
                throw std::invalid_argument("Redis numeric predicate is outside exact range");
            }
            if (columnType == ColumnInfo::ValueKind::kFloat32) {
                const auto real = static_cast<float>(number);
                if (!std::isfinite(real)) {
                    throw std::invalid_argument("Redis numeric predicate is not finite");
                }
                result = std::to_chars(std::begin(buffer), std::end(buffer), real,
                    std::chars_format::general, std::numeric_limits<float>::max_digits10);
            } else {
                result = std::to_chars(std::begin(buffer), std::end(buffer), number,
                    std::chars_format::general, std::numeric_limits<double>::max_digits10);
            }
            break;
        }
        case redis_literal_kind::boolean:
            buffer[0] = redis_literal_access::boolean_value(value) ? '1' : '0';
            result.ptr = buffer + 1;
            result.ec = std::errc{};
            break;
        default:
            throw std::invalid_argument("Redis numeric predicate requires a numeric value");
    }
    if (result.ec != std::errc{}) {
        throw std::invalid_argument("failed to format Redis numeric predicate");
    }
    return std::pmr::string(buffer, static_cast<std::size_t>(result.ptr - buffer), resource);
}

[[nodiscard]] inline std::pmr::string canonicalTag(
    const redis_literal& value, ColumnInfo::ValueKind columnType,
    std::pmr::memory_resource* resource) {
    switch (redis_literal_access::type(value)) {
        case redis_literal_kind::string:
            if (!isTextColumnKind(columnType)) {
                throw std::invalid_argument("Redis TAG predicate value type does not match column");
            }
            return encodeRedisTag(redis_literal_access::text(value), resource);
        case redis_literal_kind::boolean:
            if (columnType != ColumnInfo::ValueKind::kBool) {
                throw std::invalid_argument("Redis TAG predicate value type does not match column");
            }
            return encodeRedisTag(redis_literal_access::boolean_value(value) ? "1" : "0", resource);
        case redis_literal_kind::signed_integer:
        case redis_literal_kind::unsigned_integer:
        case redis_literal_kind::real:
            if (!isNumericColumnKind(columnType)) {
                throw std::invalid_argument("Redis TAG predicate value type does not match column");
            }
            return encodeRedisTag(canonicalNumeric(value, columnType, resource), resource);
        default:
            throw std::invalid_argument("Redis TAG predicate requires a scalar value");
    }
}

[[nodiscard]] inline std::pmr::string canonicalText(
    const redis_literal& value, std::pmr::memory_resource* resource) {
    if (redis_literal_access::type(value) != redis_literal_kind::string) {
        throw std::invalid_argument("Redis TEXT predicate requires a string value");
    }
    return std::pmr::string(redis_literal_access::text(value), resource);
}

[[nodiscard]] inline std::pmr::string canonicalPrimaryKey(
    const redis_literal& value, ColumnInfo::ValueKind columnType,
    std::pmr::memory_resource* resource) {
    if (isTextColumnKind(columnType)) {
        return canonicalText(value, resource);
    }
    if (!isIntegerColumnKind(columnType)) {
        throw std::invalid_argument("Redis primary key must be text or integer");
    }
    const auto type = redis_literal_access::type(value);
    char buffer[128]{};
    std::to_chars_result result{};
    switch (type) {
        case redis_literal_kind::signed_integer:
            result = std::to_chars(std::begin(buffer), std::end(buffer),
                redis_literal_access::signed_value(value));
            break;
        case redis_literal_kind::unsigned_integer:
            result = std::to_chars(std::begin(buffer), std::end(buffer),
                redis_literal_access::unsigned_value(value));
            break;
        case redis_literal_kind::real: {
            const double number = redis_literal_access::real_value(value);
            if (!std::isfinite(number) || std::trunc(number) != number ||
                std::fabs(static_cast<long double>(number)) > kMaxExactRedisNumber) {
                throw std::invalid_argument("Redis integer primary key is invalid");
            }
            result = std::to_chars(std::begin(buffer), std::end(buffer),
                static_cast<std::int64_t>(number));
            break;
        }
        default:
            throw std::invalid_argument("Redis primary key requires a string or integer value");
    }
    if (result.ec != std::errc{}) {
        throw std::invalid_argument("failed to format Redis primary key");
    }
    return std::pmr::string(buffer, static_cast<std::size_t>(result.ptr - buffer), resource);
}

[[nodiscard]] inline RedisIndexKind indexKind(
    const RedisMapping& mapping, std::string_view field) {
    const auto kind = mapping.indexKind(field);
    if (kind == RedisIndexKind::kNone) {
        throw std::invalid_argument("Redis query column is not indexed");
    }
    return kind;
}

[[nodiscard]] inline std::pmr::string presenceTerm(
    std::string_view field, std::pmr::memory_resource* resource) {
    const auto shadow = redisPresentShadow(field, resource);
    std::pmr::string result(resource);
    result += '@';
    result.append(shadow.data(), shadow.size());
    result += ":{";
    // Presence markers are written as the raw value 1. They are indexed as a
    // TAG shadow field, but are not passed through the user TAG encoding.
    result.append(kPresentValue.data(), kPresentValue.size());
    result += '}';
    return result;
}

inline void appendPresence(std::pmr::string& output, std::string_view field,
    std::pmr::memory_resource* resource) {
    const auto value = presenceTerm(field, resource);
    output.append(value.data(), value.size());
}

inline void appendTagComparison(std::pmr::string& output, std::string_view field,
    const redis_literal& value, ColumnInfo::ValueKind columnType, redis_binary_operator operation,
    std::pmr::memory_resource* resource) {
    const auto null = redis_literal_access::type(value) == redis_literal_kind::null;
    if (null) {
        if (operation == redis_binary_operator::equal) {
            output.push_back('-');
            appendPresence(output, field, resource);
        } else if (operation == redis_binary_operator::not_equal) {
            appendPresence(output, field, resource);
        } else {
            throw std::invalid_argument("unsupported NULL TAG comparison");
        }
        return;
    }
    const auto encoded = canonicalTag(value, columnType, resource);
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
            appendPresence(output, field, resource);
            output += " -";
            output.append(term.data(), term.size());
            output.push_back(')');
            return;
        default:
            throw std::invalid_argument("Redis TAG indexes only support equality predicates");
    }
}

inline void appendNumericComparison(std::pmr::string& output, std::string_view field,
    const redis_literal& value, ColumnInfo::ValueKind columnType, redis_binary_operator operation,
    std::pmr::memory_resource* resource) {
    const auto null = redis_literal_access::type(value) == redis_literal_kind::null;
    if (null) {
        if (operation == redis_binary_operator::equal) {
            output.push_back('-');
            appendPresence(output, field, resource);
        } else if (operation == redis_binary_operator::not_equal) {
            appendPresence(output, field, resource);
        } else {
            throw std::invalid_argument("unsupported NULL NUMERIC comparison");
        }
        return;
    }
    const auto number = canonicalNumeric(value, columnType, resource);
    const bool notEqual = operation == redis_binary_operator::not_equal;
    if (notEqual) {
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

template <typename Entity>
void appendExpression(std::pmr::string& output, redis_expression expression,
    const RedisMapping& mapping, std::pmr::memory_resource* resource,
    std::size_t depth = 0U);

template <typename Entity>
void appendIndexedComparison(std::pmr::string& output, std::string_view field,
    const redis_literal& value, redis_binary_operator operation, const RedisMapping& mapping,
    std::pmr::memory_resource* resource) {
    const auto info = columnInfo<Entity>(field);
    switch (indexKind(mapping, field)) {
        case RedisIndexKind::kTag:
            appendTagComparison(output, field, value, info.valueKind, operation, resource);
            return;
        case RedisIndexKind::kNumeric:
            appendNumericComparison(output, field, value, info.valueKind, operation, resource);
            return;
        case RedisIndexKind::kText:
            if (redis_literal_access::type(value) == redis_literal_kind::null &&
                (operation == redis_binary_operator::equal || operation == redis_binary_operator::not_equal)) {
                if (operation == redis_binary_operator::equal) {
                    output.push_back('-');
                    appendPresence(output, field, resource);
                } else {
                    appendPresence(output, field, resource);
                }
                return;
            }
            throw std::invalid_argument("Redis TEXT predicates are not exposed by redis_predicate");
        default:
            throw std::invalid_argument("invalid Redis query index kind");
    }
}

template <typename Entity>
void appendInList(std::pmr::string& output, std::string_view field,
    redis_expression list, redis_binary_operator operation, const RedisMapping& mapping,
    std::pmr::memory_resource* resource) {
    const auto listInfo = redis_expression_access::inspect(list);
    if (listInfo.kind != redis_expression_inspection::kind_type::list) {
        throw std::invalid_argument("Redis IN requires a literal list");
    }
    const auto count = redis_expression_access::operand_count(list);
    const bool negated = operation == redis_binary_operator::not_in;
    if (count == 0U) {
        if (negated) {
            output.push_back('*');
        } else {
            const auto presence = redisPresentShadow(field, resource);
            output += '@';
            output.append(presence.data(), presence.size());
            output += ":{x00}";
        }
        return;
    }
    if (negated) {
        output.push_back('(');
        appendPresence(output, field, resource);
        output += " -(";
    } else {
        output.push_back('(');
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0U) {
            output.push_back('|');
        }
        const auto item = redis_expression_access::operand(list, index);
        const auto itemInfo = redis_expression_access::inspect(item);
        if (itemInfo.kind != redis_expression_inspection::kind_type::literal || itemInfo.value == nullptr ||
            redis_literal_access::type(*itemInfo.value) == redis_literal_kind::null) {
            throw std::invalid_argument("Redis IN list requires non-null literal values");
        }
        const auto kind = indexKind(mapping, field);
        const auto info = columnInfo<Entity>(field);
        if (kind == RedisIndexKind::kTag) {
            const auto encoded = canonicalTag(*itemInfo.value, info.valueKind, resource);
            output += '@';
            output.append(field.data(), field.size());
            output += ":{";
            output.append(encoded.data(), encoded.size());
            output.push_back('}');
        } else if (kind == RedisIndexKind::kNumeric) {
            const auto number = canonicalNumeric(*itemInfo.value, info.valueKind, resource);
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

template <typename Entity>
void appendBetween(std::pmr::string& output, redis_expression expression,
    const RedisMapping& mapping, std::pmr::memory_resource* resource) {
    const auto inspection = redis_expression_access::inspect(expression);
    if (inspection.kind != redis_expression_inspection::kind_type::between ||
        redis_expression_access::operand_count(expression) != 3U) {
        throw std::invalid_argument("invalid Redis BETWEEN expression");
    }
    const auto column = redis_expression_access::operand(expression, 0);
    const auto lower = redis_expression_access::operand(expression, 1);
    const auto upper = redis_expression_access::operand(expression, 2);
    const auto columnInfoValue = redis_expression_access::inspect(column);
    const auto lowerInfo = redis_expression_access::inspect(lower);
    const auto upperInfo = redis_expression_access::inspect(upper);
    if (columnInfoValue.kind != redis_expression_inspection::kind_type::field ||
        lowerInfo.kind != redis_expression_inspection::kind_type::literal || lowerInfo.value == nullptr ||
        upperInfo.kind != redis_expression_inspection::kind_type::literal || upperInfo.value == nullptr) {
        throw std::invalid_argument("Redis BETWEEN requires a column and literal bounds");
    }
    validate_entity_prefix<Entity>(columnInfoValue.entity_prefix);
    const auto field = columnInfoValue.field;
    const auto info = columnInfo<Entity>(field);
    if (indexKind(mapping, field) != RedisIndexKind::kNumeric ||
        redis_literal_access::type(*lowerInfo.value) == redis_literal_kind::null ||
        redis_literal_access::type(*upperInfo.value) == redis_literal_kind::null) {
        throw std::invalid_argument("Redis BETWEEN requires a NUMERIC indexed column");
    }
    const auto low = canonicalNumeric(*lowerInfo.value, info.valueKind, resource);
    const auto high = canonicalNumeric(*upperInfo.value, info.valueKind, resource);
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

template <typename Entity>
void appendExpression(std::pmr::string& output, redis_expression expression,
    const RedisMapping& mapping, std::pmr::memory_resource* resource,
    std::size_t depth) {
    constexpr std::size_t maxDepth = 64U;
    if (depth > maxDepth) {
        throw std::invalid_argument("Redis predicate nesting is too deep");
    }
    const auto inspection = redis_expression_access::inspect(expression);
    using Kind = redis_expression_inspection::kind_type;
    if (inspection.kind == Kind::between) {
        appendBetween<Entity>(output, expression, mapping, resource);
        return;
    }
    if (inspection.kind == Kind::binary) {
        if (inspection.binary == redis_binary_operator::logical_and ||
            inspection.binary == redis_binary_operator::logical_or) {
            if (redis_expression_access::operand_count(expression) != 2U) {
                throw std::invalid_argument("invalid Redis logical expression");
            }
            output.push_back('(');
            appendExpression<Entity>(output, redis_expression_access::operand(expression, 0), mapping, resource, depth + 1U);
            output.push_back(inspection.binary == redis_binary_operator::logical_and ? ' ' : '|');
            appendExpression<Entity>(output, redis_expression_access::operand(expression, 1), mapping, resource, depth + 1U);
            output.push_back(')');
            return;
        }
        if (inspection.binary == redis_binary_operator::in ||
            inspection.binary == redis_binary_operator::not_in) {
            if (redis_expression_access::operand_count(expression) != 2U) {
                throw std::invalid_argument("invalid Redis IN expression");
            }
            const auto left = redis_expression_access::inspect(redis_expression_access::operand(expression, 0));
            if (left.kind != Kind::field) {
                throw std::invalid_argument("Redis IN requires a column");
            }
            validate_entity_prefix<Entity>(left.entity_prefix);
            (void)columnInfo<Entity>(left.field);
            appendInList<Entity>(output, left.field,
                redis_expression_access::operand(expression, 1), inspection.binary, mapping, resource);
            return;
        }
        if (inspection.binary != redis_binary_operator::equal &&
            inspection.binary != redis_binary_operator::not_equal &&
            inspection.binary != redis_binary_operator::less &&
            inspection.binary != redis_binary_operator::less_equal &&
            inspection.binary != redis_binary_operator::greater &&
            inspection.binary != redis_binary_operator::greater_equal) {
            throw std::invalid_argument("unsupported Redis predicate operator");
        }
        if (redis_expression_access::operand_count(expression) != 2U) {
            throw std::invalid_argument("invalid Redis comparison expression");
        }
        const auto leftExpression = redis_expression_access::operand(expression, 0);
        const auto rightExpression = redis_expression_access::operand(expression, 1);
        const auto left = redis_expression_access::inspect(leftExpression);
        const auto right = redis_expression_access::inspect(rightExpression);
        redis_expression_inspection column{};
        const redis_literal* value = nullptr;
        auto operation = inspection.binary;
        if (left.kind == Kind::field && right.kind == Kind::literal) {
            column = left;
            value = right.value;
        } else if (right.kind == Kind::field && left.kind == Kind::literal) {
            column = right;
            value = left.value;
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
        validate_entity_prefix<Entity>(column.entity_prefix);
        appendIndexedComparison<Entity>(output, column.field, *value,
            operation, mapping, resource);
        return;
    }
    throw std::invalid_argument("unsupported Redis expression");
}

template <typename Entity>
[[nodiscard]] std::pmr::string sortField(std::string_view field,
    const RedisMapping& mapping, std::pmr::memory_resource* resource) {
    (void)columnInfo<Entity>(field);
    if (!mapping.sortable(field)) {
        throw std::invalid_argument("Redis order column is not sortable");
    }
    if (mapping.indexKind(field) == RedisIndexKind::kTag) {
        return redisSortShadow(field, resource);
    }
    if (mapping.indexKind(field) == RedisIndexKind::kNone) {
        throw std::invalid_argument("Redis order column is not indexed");
    }
    return std::pmr::string(field.data(), field.size(), resource);
}

}  // namespace redis_query_detail

template <typename Entity>
[[nodiscard]] std::pmr::vector<std::pmr::string> compileRedisFind(
    const redis_find_options& options, const RedisMapping& mapping,
    std::pmr::memory_resource* resource, bool countOnly = false,
    std::optional<std::uint64_t> takeOverride = {}) {
    resource = pmrResourceOrDefault(resource);
    std::pmr::vector<std::pmr::string> args(resource);
    const auto index = redis_query_detail::redisIndexName(mapping.prefix, resource);
    redis_query_detail::appendArgument(args, "FT.SEARCH");
    redis_query_detail::appendArgument(args, index);
    std::pmr::string query(resource);
    const auto root = redis_predicate_access::root(options.where);
    if (root.empty()) {
        query = "*";
    } else {
        redis_query_detail::appendExpression<Entity>(query, root, mapping, resource);
    }
    args.emplace_back(std::move(query));
    redis_query_detail::appendArgument(args, "LIMIT");
    if (countOnly) {
        redis_query_detail::appendArgument(args, "0");
        redis_query_detail::appendArgument(args, "0");
    } else {
        constexpr auto maxRedisInteger = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const auto skip = options.skip.value_or(0U);
        const auto take = takeOverride.value_or(options.take.value_or(100U));
        if (skip > maxRedisInteger || take > maxRedisInteger) {
            throw std::invalid_argument("Redis Search LIMIT exceeds signed 64-bit range");
        }
        std::pmr::string encodedSkip(resource), encodedTake(resource);
        redis_query_detail::appendUnsigned(encodedSkip, skip);
        redis_query_detail::appendUnsigned(encodedTake, take);
        args.emplace_back(std::move(encodedSkip));
        args.emplace_back(std::move(encodedTake));
    }
    if (!countOnly && !options.order.empty()) {
        if (options.order.size() != 1U) {
            throw std::invalid_argument("Redis Search supports one SORTBY column per query");
        }
        const auto& order = options.order.front();
        const auto field = redis_query_detail::sortField<Entity>(order.field, mapping, resource);
        redis_query_detail::appendArgument(args, "SORTBY");
        redis_query_detail::appendArgument(args, field);
        switch (order.direction) {
            case redis_order_direction::ascending:
                redis_query_detail::appendArgument(args, "ASC");
                break;
            case redis_order_direction::descending:
                redis_query_detail::appendArgument(args, "DESC");
                break;
            default:
                throw std::invalid_argument("invalid Redis order direction");
        }
    }
    redis_query_detail::appendArgument(args, "DIALECT");
    redis_query_detail::appendArgument(args, "2");
    return args;
}

template <typename Entity>
[[nodiscard]] std::pmr::vector<std::pmr::string> compileRedisIndex(
    const RedisMapping& mapping, std::pmr::memory_resource* resource) {
    resource = pmrResourceOrDefault(resource);
    std::pmr::vector<std::pmr::string> args(resource);
    const auto index = redis_query_detail::redisIndexName(mapping.prefix, resource);
    const auto keyPrefix = redisEntityStoragePrefix(mapping.prefix, resource);
    redis_query_detail::appendArgument(args, "FT.CREATE");
    redis_query_detail::appendArgument(args, index);
    redis_query_detail::appendArgument(args, "ON");
    redis_query_detail::appendArgument(args, "HASH");
    redis_query_detail::appendArgument(args, "PREFIX");
    redis_query_detail::appendArgument(args, "1");
    redis_query_detail::appendArgument(args, keyPrefix);
    redis_query_detail::appendArgument(args, "SCHEMA");
    if (mapping.indexes.empty()) {
        throw std::invalid_argument("Redis repository requires at least one index column");
    }
    std::size_t schemaCount = 0U;
    for (const auto& definition : mapping.indexes) {
        const auto field = std::string_view(definition.field);
        (void)redis_query_detail::columnInfo<Entity>(field);
        if (!redis_query_detail::isSafeIdentifier(field)) {
            throw std::invalid_argument("invalid Redis index column name");
        }
        if (definition.kind == RedisIndexKind::kNone ||
            mapping.indexKind(field) != definition.kind ||
            mapping.sortable(field) != definition.sortable) {
            throw std::invalid_argument("Redis mapping index metadata is inconsistent");
        }
        if (field.starts_with("__ruvia_")) {
            throw std::invalid_argument("Redis index column uses reserved __ruvia_ namespace");
        }
        for (const auto& other : mapping.indexes) {
            if (&other != &definition && other.field == definition.field) {
                throw std::invalid_argument("duplicate Redis index column");
            }
        }
        const auto valueKind = redis_query_detail::columnInfo<Entity>(field).valueKind;
        if ((definition.kind == RedisIndexKind::kTag &&
                valueKind != redis_query_detail::ColumnInfo::ValueKind::kBool &&
                !redis_query_detail::isTextColumnKind(valueKind)) ||
            (definition.kind == RedisIndexKind::kText &&
                !redis_query_detail::isTextColumnKind(valueKind)) ||
            (definition.kind == RedisIndexKind::kNumeric &&
                !redis_query_detail::isNumericColumnKind(valueKind))) {
            throw std::invalid_argument("Redis index kind does not match entity column type");
        }
        const auto presence = redis_query_detail::redisPresentShadow(field, resource);
        switch (definition.kind) {
            case RedisIndexKind::kTag: {
                const auto shadow = redis_query_detail::redisTagShadow(field, resource);
                redis_query_detail::appendArgument(args, shadow);
                redis_query_detail::appendArgument(args, "AS");
                redis_query_detail::appendArgument(args, field);
                redis_query_detail::appendArgument(args, "TAG");
                redis_query_detail::appendArgument(args, "CASESENSITIVE");
                ++schemaCount;
                break;
            }
            case RedisIndexKind::kText:
                redis_query_detail::appendArgument(args, field);
                redis_query_detail::appendArgument(args, "TEXT");
                if (definition.sortable) {
                    redis_query_detail::appendArgument(args, "SORTABLE");
                }
                ++schemaCount;
                break;
            case RedisIndexKind::kNumeric:
                redis_query_detail::appendArgument(args, field);
                redis_query_detail::appendArgument(args, "NUMERIC");
                if (definition.sortable) {
                    redis_query_detail::appendArgument(args, "SORTABLE");
                }
                ++schemaCount;
                break;
            default:
                throw std::invalid_argument("invalid Redis index kind");
        }
        redis_query_detail::appendArgument(args, presence);
        redis_query_detail::appendArgument(args, "AS");
        redis_query_detail::appendArgument(args, presence);
        redis_query_detail::appendArgument(args, "TAG");
        redis_query_detail::appendArgument(args, "CASESENSITIVE");
        ++schemaCount;
        if (definition.kind == RedisIndexKind::kTag && definition.sortable) {
            const auto sortShadow = redis_query_detail::redisSortShadow(field, resource);
            redis_query_detail::appendArgument(args, field);
            redis_query_detail::appendArgument(args, "AS");
            redis_query_detail::appendArgument(args, sortShadow);
            redis_query_detail::appendArgument(args, "TAG");
            redis_query_detail::appendArgument(args, "SORTABLE");
            redis_query_detail::appendArgument(args, "NOINDEX");
            ++schemaCount;
        }
    }
    if (schemaCount == 0U) {
        throw std::invalid_argument("Redis repository requires an indexed column");
    }
    return args;
}

template <typename Entity>
[[nodiscard]] std::optional<std::pmr::string> redisPrimaryKey(
    const redis_predicate& predicate, std::pmr::memory_resource* resource) {
    resource = pmrResourceOrDefault(resource);
    const auto root = redis_predicate_access::root(predicate);
    if (root.empty()) {
        return std::nullopt;
    }
    const auto rootInfo = redis_expression_access::inspect(root);
    if (rootInfo.kind != redis_expression_inspection::kind_type::binary ||
        rootInfo.binary != redis_binary_operator::equal ||
        redis_expression_access::operand_count(root) != 2U) {
        return std::nullopt;
    }
    const auto leftExpression = redis_expression_access::operand(root, 0);
    const auto rightExpression = redis_expression_access::operand(root, 1);
    const auto left = redis_expression_access::inspect(leftExpression);
    const auto right = redis_expression_access::inspect(rightExpression);
    redis_expression_inspection column{};
    const redis_literal* value = nullptr;
    if (left.kind == redis_expression_inspection::kind_type::field && right.kind == redis_expression_inspection::kind_type::literal) {
        column = left;
        value = right.value;
    } else if (right.kind == redis_expression_inspection::kind_type::field && left.kind == redis_expression_inspection::kind_type::literal) {
        column = right;
        value = left.value;
    } else {
        return std::nullopt;
    }
    redis_query_detail::validate_entity_prefix<Entity>(column.entity_prefix);
    const auto info = redis_query_detail::columnInfo<Entity>(column.field);
    if (!info.primaryKey) {
        return std::nullopt;
    }
    if (value == nullptr || redis_literal_access::type(*value) == redis_literal_kind::null) {
        throw std::invalid_argument("Redis primary key cannot be NULL");
    }
    if (!redis_query_detail::isTextColumnKind(info.valueKind) &&
        !redis_query_detail::isIntegerColumnKind(info.valueKind)) {
        throw std::invalid_argument("Redis primary key must be text or integer");
    }
    auto result = redis_query_detail::canonicalPrimaryKey(*value, info.valueKind, resource);
    if (result.empty()) {
        throw std::invalid_argument("Redis primary key cannot be empty");
    }
    return result;
}

}  // namespace ruvia::detail
