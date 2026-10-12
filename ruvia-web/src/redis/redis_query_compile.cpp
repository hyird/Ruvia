#include "ruvia/web/detail/redis/redis_query_compile.h"

#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/detail/redis/redis_entity_key.h"
#include "ruvia/web/redis/redis_predicate.h"

namespace ruvia::detail {
namespace {

constexpr long double max_exact_redis_number = 9007199254740991.0L;
constexpr std::string_view present_value = "1";

[[nodiscard]] bool is_safe_identifier(std::string_view value) noexcept {
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

void append_argument(std::pmr::vector<std::pmr::string>& args, std::string_view value) {
    args.emplace_back(value.data(), value.size());
}

[[nodiscard]] std::pmr::string redis_index_name(std::string_view prefix, std::pmr::memory_resource* resource) {
    if (prefix.empty() || prefix.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("invalid Redis entity prefix");
    }
    std::pmr::string result_value(prefix.data(), prefix.size(), resource);
    result_value += ":idx";
    return result_value;
}

// Shadow hash fields share the reserved __ruvia_ namespace with the entity
// marker. Query and index compilation validate the column name first; entity
// writes use the declared column names as they are.
[[nodiscard]] std::pmr::string shadow_field(
    std::string_view shadow, std::string_view field, std::pmr::memory_resource* resource) {
    std::pmr::string result_value(shadow, resource);
    result_value.append(field.data(), field.size());
    return result_value;
}

constexpr std::string_view tag_shadow = "__ruvia_tag_";
constexpr std::string_view sort_shadow = "__ruvia_sort_";
constexpr std::string_view present_shadow = "__ruvia_present_";

[[nodiscard]] std::pmr::string checked_shadow_field(
    std::string_view shadow, std::string_view field, std::pmr::memory_resource* resource) {
    if (!is_safe_identifier(field)) {
        throw std::invalid_argument("invalid Redis entity column name");
    }
    return shadow_field(shadow, field, resource);
}

[[nodiscard]] const redis_column_info& find_column_info(
    std::span<const redis_column_info> columns, std::string_view name) {
    const redis_column_info* result = nullptr;
    for (const auto& column : columns) {
        if (column.name_ == name) {
            result = &column;
        }
    }
    if (result == nullptr) {
        throw std::invalid_argument("unknown Redis query column");
    }
    if (!is_safe_identifier(name)) {
        throw std::invalid_argument("invalid Redis query column name");
    }
    return *result;
}

void validate_entity_prefix(std::string_view entity_prefix, std::string_view table_value) {
    if (!table_value.empty() && table_value != entity_prefix) {
        throw std::invalid_argument("Redis predicate belongs to another entity prefix");
    }
}

[[nodiscard]] bool is_numeric_column_kind(redis_column_kind kind) noexcept {
    return kind == redis_column_kind::signed_value ||
           kind == redis_column_kind::unsigned_value ||
           kind == redis_column_kind::float32 ||
           kind == redis_column_kind::float64;
}

[[nodiscard]] bool is_integer_column_kind(redis_column_kind kind) noexcept {
    return kind == redis_column_kind::signed_value ||
           kind == redis_column_kind::unsigned_value;
}

[[nodiscard]] bool is_text_column_kind(redis_column_kind kind) noexcept {
    return kind == redis_column_kind::text;
}

[[nodiscard]] std::pmr::string canonical_numeric(
    const redis_literal& value, redis_column_kind column_kind, std::pmr::memory_resource* resource) {
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
            std::pmr::string text(resource);
            http_append_formatted_number(text, number, "failed to format Redis numeric predicate");
            return text;
        }
        case redis_literal_kind::unsigned_integer: {
            const auto number = redis_literal_access::unsigned_value(value);
            if (static_cast<long double>(number) > max_exact_redis_number) {
                throw std::invalid_argument("Redis numeric predicate exceeds exact range");
            }
            std::pmr::string text(resource);
            http_append_unsigned(text, number);
            return text;
        }
        case redis_literal_kind::real: {
            double number = redis_literal_access::real_value(value);
            if (!std::isfinite(number) ||
                std::fabs(static_cast<long double>(number)) > max_exact_redis_number) {
                throw std::invalid_argument("Redis numeric predicate is outside exact range");
            }
            if (column_kind == redis_column_kind::float32) {
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

[[nodiscard]] std::pmr::string canonical_tag(
    const redis_literal& value, redis_column_kind column_kind, std::pmr::memory_resource* resource) {
    switch (redis_literal_access::type(value)) {
        case redis_literal_kind::string:
            if (!is_text_column_kind(column_kind)) {
                throw std::invalid_argument("Redis TAG predicate value type does not match column");
            }
            return encode_redis_tag(redis_literal_access::text(value), resource);
        case redis_literal_kind::boolean:
            if (column_kind != redis_column_kind::bool_value) {
                throw std::invalid_argument("Redis TAG predicate value type does not match column");
            }
            return encode_redis_tag(redis_literal_access::boolean_value(value) ? "1" : "0", resource);
        case redis_literal_kind::signed_integer:
        case redis_literal_kind::unsigned_integer:
        case redis_literal_kind::real:
            if (!is_numeric_column_kind(column_kind)) {
                throw std::invalid_argument("Redis TAG predicate value type does not match column");
            }
            return encode_redis_tag(canonical_numeric(value, column_kind, resource), resource);
        default:
            throw std::invalid_argument("Redis TAG predicate requires a scalar value");
    }
}

[[nodiscard]] std::pmr::string canonical_primary_key(
    const redis_literal& value, redis_column_kind column_kind, std::pmr::memory_resource* resource) {
    if (is_text_column_kind(column_kind)) {
        if (redis_literal_access::type(value) != redis_literal_kind::string) {
            throw std::invalid_argument("Redis TEXT predicate requires a string value");
        }
        return std::pmr::string(redis_literal_access::text(value), resource);
    }
    if (!is_integer_column_kind(column_kind)) {
        throw std::invalid_argument("Redis primary key must be text or integer");
    }
    std::pmr::string result_value(resource);
    switch (redis_literal_access::type(value)) {
        case redis_literal_kind::signed_integer:
            http_append_formatted_number(result_value, redis_literal_access::signed_value(value),
                "failed to format Redis primary key");
            break;
        case redis_literal_kind::unsigned_integer:
            http_append_unsigned(result_value, redis_literal_access::unsigned_value(value));
            break;
        case redis_literal_kind::real: {
            const double number = redis_literal_access::real_value(value);
            if (!std::isfinite(number) || std::trunc(number) != number ||
                std::fabs(static_cast<long double>(number)) > max_exact_redis_number) {
                throw std::invalid_argument("Redis integer primary key is invalid");
            }
            http_append_formatted_number(result_value, static_cast<std::int64_t>(number),
                "failed to format Redis primary key");
            break;
        }
        default:
            throw std::invalid_argument("Redis primary key requires a string or integer value");
    }
    return result_value;
}

[[nodiscard]] redis_index_kind index_kind(const redis_mapping& mapping, std::string_view field) {
    const auto kind = mapping.index_kind(field);
    if (kind == redis_index_kind::none) {
        throw std::invalid_argument("Redis query column is not indexed");
    }
    return kind;
}

void append_presence(std::pmr::string& output, std::string_view field, std::pmr::memory_resource* resource) {
    const auto shadow = checked_shadow_field(present_shadow, field, resource);
    output += '@';
    output.append(shadow.data(), shadow.size());
    output += ":{";
    // Presence markers are written as the raw value 1. They are indexed as a
    // TAG shadow field, but are not passed through the user TAG encoding.
    output.append(present_value.data(), present_value.size());
    output += '}';
}

// `field IS NULL` / `field IS NOT NULL` through the presence shadow.
void append_null_comparison(std::pmr::string& output, std::string_view field,
    redis_binary_operator operation, std::string_view unsupported, std::pmr::memory_resource* resource) {
    if (operation == redis_binary_operator::equal) {
        output.push_back('-');
        append_presence(output, field, resource);
    } else if (operation == redis_binary_operator::not_equal) {
        append_presence(output, field, resource);
    } else {
        throw std::invalid_argument(std::string(unsupported));
    }
}

void append_tag_comparison(std::pmr::string& output, std::string_view field, const redis_literal& value,
    redis_column_kind column_kind, redis_binary_operator operation, std::pmr::memory_resource* resource) {
    if (redis_literal_access::type(value) == redis_literal_kind::null) {
        append_null_comparison(output, field, operation, "unsupported NULL TAG comparison", resource);
        return;
    }
    const auto encoded = canonical_tag(value, column_kind, resource);
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

void append_numeric_comparison(std::pmr::string& output, std::string_view field, const redis_literal& value,
    redis_column_kind column_kind, redis_binary_operator operation, std::pmr::memory_resource* resource) {
    if (redis_literal_access::type(value) == redis_literal_kind::null) {
        append_null_comparison(output, field, operation, "unsupported NULL NUMERIC comparison", resource);
        return;
    }
    const auto number = canonical_numeric(value, column_kind, resource);
    if (operation == redis_binary_operator::not_equal) {
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

// Compiles one predicate tree against one entity's columns and index mapping.
class redis_query_compiler final {
public:
    redis_query_compiler(std::span<const redis_column_info> columns, std::string_view entity_prefix,
        const redis_mapping& mapping, std::pmr::memory_resource* resource) noexcept
        : columns_(columns),
          entity_prefix_(entity_prefix),
          mapping_(mapping),
          resource_(resource) {}

    void append_expression(std::pmr::string& output, redis_expression expression, std::size_t depth = 0U) const {
        constexpr std::size_t max_depth = 64U;
        if (depth > max_depth) {
            throw std::invalid_argument("Redis predicate nesting is too deep");
        }
        const auto inspection = redis_expression_access::inspect(expression);
        using kind_type = redis_expression_inspection::kind_type;
        if (inspection.kind_ == kind_type::between) {
            append_between(output, expression);
            return;
        }
        if (inspection.kind_ != kind_type::binary) {
            throw std::invalid_argument("unsupported Redis expression");
        }
        if (inspection.binary_ == redis_binary_operator::logical_and ||
            inspection.binary_ == redis_binary_operator::logical_or) {
            if (redis_expression_access::operand_count(expression) != 2U) {
                throw std::invalid_argument("invalid Redis logical expression");
            }
            output.push_back('(');
            append_expression(output, redis_expression_access::operand(expression, 0), depth + 1U);
            output.push_back(inspection.binary_ == redis_binary_operator::logical_and ? ' ' : '|');
            append_expression(output, redis_expression_access::operand(expression, 1), depth + 1U);
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
            validate_entity_prefix(entity_prefix_, left.entity_prefix_);
            (void)find_column_info(columns_, left.field_);
            append_in_list(output, left.field_, redis_expression_access::operand(expression, 1), inspection.binary_);
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
        const auto left = redis_expression_access::inspect(redis_expression_access::operand(expression, 0));
        const auto right = redis_expression_access::inspect(redis_expression_access::operand(expression, 1));
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
        validate_entity_prefix(entity_prefix_, column.entity_prefix_);
        append_indexed_comparison(output, column.field_, *value, operation);
    }

    [[nodiscard]] std::pmr::string sort_field(std::string_view field) const {
        (void)find_column_info(columns_, field);
        if (!mapping_.sortable(field)) {
            throw std::invalid_argument("Redis order column is not sortable");
        }
        if (mapping_.index_kind(field) == redis_index_kind::tag) {
            return checked_shadow_field(sort_shadow, field, resource_);
        }
        if (mapping_.index_kind(field) == redis_index_kind::none) {
            throw std::invalid_argument("Redis order column is not indexed");
        }
        return std::pmr::string(field.data(), field.size(), resource_);
    }

private:
    void append_indexed_comparison(std::pmr::string& output, std::string_view field,
        const redis_literal& value, redis_binary_operator operation) const {
        const auto& info = find_column_info(columns_, field);
        switch (index_kind(mapping_, field)) {
            case redis_index_kind::tag:
                append_tag_comparison(output, field, value, info.kind_, operation, resource_);
                return;
            case redis_index_kind::numeric:
                append_numeric_comparison(output, field, value, info.kind_, operation, resource_);
                return;
            case redis_index_kind::text:
                if (redis_literal_access::type(value) == redis_literal_kind::null &&
                    (operation == redis_binary_operator::equal || operation == redis_binary_operator::not_equal)) {
                    append_null_comparison(output, field, operation, {}, resource_);
                    return;
                }
                throw std::invalid_argument("Redis TEXT predicates are not exposed by redis_predicate");
            default:
                throw std::invalid_argument("invalid Redis query index kind");
        }
    }

    void append_in_list(std::pmr::string& output, std::string_view field,
        redis_expression list, redis_binary_operator operation) const {
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
                const auto presence = checked_shadow_field(present_shadow, field, resource_);
                output += '@';
                output.append(presence.data(), presence.size());
                output += ":{x00}";
            }
            return;
        }
        if (negated) {
            output.push_back('(');
            append_presence(output, field, resource_);
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
            const auto kind = index_kind(mapping_, field);
            const auto& info = find_column_info(columns_, field);
            if (kind == redis_index_kind::tag) {
                const auto encoded = canonical_tag(*item_info.value_, info.kind_, resource_);
                output += '@';
                output.append(field.data(), field.size());
                output += ":{";
                output.append(encoded.data(), encoded.size());
                output.push_back('}');
            } else if (kind == redis_index_kind::numeric) {
                const auto number = canonical_numeric(*item_info.value_, info.kind_, resource_);
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

    void append_between(std::pmr::string& output, redis_expression expression) const {
        const auto inspection = redis_expression_access::inspect(expression);
        if (inspection.kind_ != redis_expression_inspection::kind_type::between ||
            redis_expression_access::operand_count(expression) != 3U) {
            throw std::invalid_argument("invalid Redis BETWEEN expression");
        }
        const auto column = redis_expression_access::inspect(redis_expression_access::operand(expression, 0));
        const auto lower = redis_expression_access::inspect(redis_expression_access::operand(expression, 1));
        const auto upper = redis_expression_access::inspect(redis_expression_access::operand(expression, 2));
        if (column.kind_ != redis_expression_inspection::kind_type::field ||
            lower.kind_ != redis_expression_inspection::kind_type::literal || lower.value_ == nullptr ||
            upper.kind_ != redis_expression_inspection::kind_type::literal || upper.value_ == nullptr) {
            throw std::invalid_argument("Redis BETWEEN requires a column and literal bounds");
        }
        validate_entity_prefix(entity_prefix_, column.entity_prefix_);
        const auto field = column.field_;
        const auto& info = find_column_info(columns_, field);
        if (index_kind(mapping_, field) != redis_index_kind::numeric ||
            redis_literal_access::type(*lower.value_) == redis_literal_kind::null ||
            redis_literal_access::type(*upper.value_) == redis_literal_kind::null) {
            throw std::invalid_argument("Redis BETWEEN requires a NUMERIC indexed column");
        }
        const auto low = canonical_numeric(*lower.value_, info.kind_, resource_);
        const auto high = canonical_numeric(*upper.value_, info.kind_, resource_);
        output += '@';
        output.append(field.data(), field.size());
        output += ":[";
        output.append(low.data(), low.size());
        output.push_back(' ');
        output.append(high.data(), high.size());
        output.push_back(']');
    }

    std::span<const redis_column_info> columns_;
    std::string_view entity_prefix_;
    const redis_mapping& mapping_;
    std::pmr::memory_resource* resource_;
};

void validate_numeric_search_value(std::string_view value) {
    double number = 0.0;
    const auto parsed_value = std::from_chars(value.data(), value.data() + value.size(), number,
        std::chars_format::general);
    if (parsed_value.ec != std::errc{} || parsed_value.ptr != value.data() + value.size() ||
        !std::isfinite(number) || std::abs(number) > 9007199254740991.0) {
        throw std::invalid_argument(
            "numeric search values must be finite and within the exact integer range of Redis Search");
    }
}

void append_set(redis_orm_arguments_type& args, std::string_view field, std::string_view value) {
    args.emplace_back("s");
    args.emplace_back(field);
    args.emplace_back(value);
}

void append_delete(redis_orm_arguments_type& args, std::string_view field) {
    args.emplace_back("d");
    args.emplace_back(field);
    args.emplace_back();
}

}  // namespace

std::pmr::string encode_redis_tag(std::string_view value, std::pmr::memory_resource* resource) {
    std::pmr::string encoded(pmr_resource_or_default(resource));
    encoded.reserve(value.size() * 2U + 1U);
    encoded.push_back('x');
    http_append_lower_hex(encoded, value);
    return encoded;
}

std::pmr::vector<std::pmr::string> compile_redis_find(std::span<const redis_column_info> columns,
    std::string_view entity_prefix, const redis_find_options& options, const redis_mapping& mapping,
    std::pmr::memory_resource* resource, bool count_only, std::optional<std::uint64_t> take_override) {
    resource = pmr_resource_or_default(resource);
    const redis_query_compiler compiler(columns, entity_prefix, mapping, resource);
    std::pmr::vector<std::pmr::string> args(resource);
    append_argument(args, "FT.SEARCH");
    args.push_back(redis_index_name(mapping.prefix_, resource));
    std::pmr::string query(resource);
    const auto root = redis_predicate_access::root(options.where_);
    if (root.empty()) {
        query = "*";
    } else {
        compiler.append_expression(query, root);
    }
    args.emplace_back(std::move(query));
    append_argument(args, "LIMIT");
    if (count_only) {
        append_argument(args, "0");
        append_argument(args, "0");
    } else {
        constexpr auto max_redis_integer = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const auto skip = options.skip_.value_or(0U);
        const auto take = take_override.value_or(options.take_.value_or(redis_find_default_take));
        if (skip > max_redis_integer || take > max_redis_integer) {
            throw std::invalid_argument("Redis Search LIMIT exceeds signed 64-bit range");
        }
        std::pmr::string encoded_skip(resource), encoded_take(resource);
        http_append_unsigned(encoded_skip, skip);
        http_append_unsigned(encoded_take, take);
        args.emplace_back(std::move(encoded_skip));
        args.emplace_back(std::move(encoded_take));
    }
    if (!count_only && !options.order_.empty()) {
        if (options.order_.size() != 1U) {
            throw std::invalid_argument("Redis Search supports one SORTBY column per query");
        }
        const auto& order = options.order_.front();
        auto field = compiler.sort_field(order.field_);
        append_argument(args, "SORTBY");
        args.push_back(std::move(field));
        switch (order.direction_) {
            case redis_order_direction::ascending:
                append_argument(args, "ASC");
                break;
            case redis_order_direction::descending:
                append_argument(args, "DESC");
                break;
            default:
                throw std::invalid_argument("invalid Redis order direction");
        }
    }
    append_argument(args, "DIALECT");
    append_argument(args, "2");
    return args;
}

std::pmr::vector<std::pmr::string> compile_redis_index(std::span<const redis_column_info> columns,
    const redis_mapping& mapping, std::pmr::memory_resource* resource) {
    resource = pmr_resource_or_default(resource);
    std::pmr::vector<std::pmr::string> args(resource);
    append_argument(args, "FT.CREATE");
    args.push_back(redis_index_name(mapping.prefix_, resource));
    append_argument(args, "ON");
    append_argument(args, "HASH");
    append_argument(args, "PREFIX");
    append_argument(args, "1");
    args.push_back(redis_entity_storage_prefix(mapping.prefix_, resource));
    append_argument(args, "SCHEMA");
    if (mapping.indexes_.empty()) {
        throw std::invalid_argument("Redis repository requires at least one index column");
    }
    std::size_t schema_count = 0U;
    for (const auto& definition : mapping.indexes_) {
        const auto field = std::string_view(definition.field_);
        const auto value_kind = find_column_info(columns, field).kind_;
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
        if ((definition.kind_ == redis_index_kind::tag &&
                value_kind != redis_column_kind::bool_value && !is_text_column_kind(value_kind)) ||
            (definition.kind_ == redis_index_kind::text && !is_text_column_kind(value_kind)) ||
            (definition.kind_ == redis_index_kind::numeric && !is_numeric_column_kind(value_kind))) {
            throw std::invalid_argument("Redis index kind does not match entity column type");
        }
        const auto presence = shadow_field(present_shadow, field, resource);
        switch (definition.kind_) {
            case redis_index_kind::tag:
                args.push_back(shadow_field(tag_shadow, field, resource));
                append_argument(args, "AS");
                append_argument(args, field);
                append_argument(args, "TAG");
                append_argument(args, "CASESENSITIVE");
                ++schema_count;
                break;
            case redis_index_kind::text:
                append_argument(args, field);
                append_argument(args, "TEXT");
                if (definition.sortable_) {
                    append_argument(args, "SORTABLE");
                }
                ++schema_count;
                break;
            case redis_index_kind::numeric:
                append_argument(args, field);
                append_argument(args, "NUMERIC");
                if (definition.sortable_) {
                    append_argument(args, "SORTABLE");
                }
                ++schema_count;
                break;
            default:
                throw std::invalid_argument("invalid Redis index kind");
        }
        append_argument(args, presence);
        append_argument(args, "AS");
        append_argument(args, presence);
        append_argument(args, "TAG");
        append_argument(args, "CASESENSITIVE");
        ++schema_count;
        if (definition.kind_ == redis_index_kind::tag && definition.sortable_) {
            append_argument(args, field);
            append_argument(args, "AS");
            args.push_back(shadow_field(sort_shadow, field, resource));
            append_argument(args, "TAG");
            append_argument(args, "SORTABLE");
            append_argument(args, "NOINDEX");
            ++schema_count;
        }
    }
    if (schema_count == 0U) {
        throw std::invalid_argument("Redis repository requires an indexed column");
    }
    return args;
}

std::optional<std::pmr::string> redis_primary_key(std::span<const redis_column_info> columns,
    std::string_view entity_prefix, const redis_predicate& predicate, std::pmr::memory_resource* resource) {
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
    const auto left = redis_expression_access::inspect(redis_expression_access::operand(root, 0));
    const auto right = redis_expression_access::inspect(redis_expression_access::operand(root, 1));
    redis_expression_inspection column{};
    const redis_literal* value = nullptr;
    if (left.kind_ == redis_expression_inspection::kind_type::field &&
        right.kind_ == redis_expression_inspection::kind_type::literal) {
        column = left;
        value = right.value_;
    } else if (right.kind_ == redis_expression_inspection::kind_type::field &&
               left.kind_ == redis_expression_inspection::kind_type::literal) {
        column = right;
        value = left.value_;
    } else {
        return std::nullopt;
    }
    validate_entity_prefix(entity_prefix, column.entity_prefix_);
    const auto& info = find_column_info(columns, column.field_);
    if (!info.primary_key_) {
        return std::nullopt;
    }
    if (value == nullptr || redis_literal_access::type(*value) == redis_literal_kind::null) {
        throw std::invalid_argument("Redis primary key cannot be NULL");
    }
    auto result_value = canonical_primary_key(*value, info.kind_, resource);
    if (result_value.empty()) {
        throw std::invalid_argument("Redis primary key cannot be empty");
    }
    return result_value;
}

void append_redis_entity_field(redis_orm_arguments_type& args, std::string_view field,
    std::optional<std::string_view> value, bool numeric_index, bool tag_shadow_value,
    std::pmr::memory_resource* resource) {
    const auto presence = shadow_field(present_shadow, field, resource);
    if (!value) {
        append_delete(args, field);
        append_delete(args, presence);
        if (tag_shadow_value) {
            append_delete(args, shadow_field(tag_shadow, field, resource));
        }
        return;
    }
    if (numeric_index) {
        validate_numeric_search_value(*value);
    }
    append_set(args, field, *value);
    append_set(args, presence, present_value);
    if (tag_shadow_value) {
        append_set(args, shadow_field(tag_shadow, field, resource), encode_redis_tag(*value, resource));
    }
}

}  // namespace ruvia::detail
