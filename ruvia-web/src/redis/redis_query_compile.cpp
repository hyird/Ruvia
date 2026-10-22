#include "ruvia/web/detail/redis/redis_query_compile.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/detail/redis/redis_entity_key.h"
#include "ruvia/web/redis/redis_predicate.h"
#include "ruvia/web/redis/redis_repository.h"

#include "redis/redis_utils.h"

namespace ruvia::detail {
namespace {

// RediSearch NUMERIC fields are doubles: 2^53 - 1 is the largest magnitude
// below which every integer is stored exactly, so predicate literals and
// written values both stay within it.
constexpr double redis_max_exact_integer = 9007199254740991.0;
constexpr std::string_view present_value = "1";

constexpr std::string_view tag_shadow = "__ruvia_tag_";
constexpr std::string_view sort_shadow = "__ruvia_sort_";
constexpr std::string_view present_shadow = "__ruvia_present_";

using node_kind = redis_predicate_node::kind_type;

[[noreturn]] void throw_inexact_redis_number() {
    throw std::invalid_argument(
        "numeric search values must be finite and within the exact integer range of Redis Search");
}

void require_redis_exact_number(double value) {
    if (!std::isfinite(value) || std::fabs(value) > redis_max_exact_integer) {
        throw_inexact_redis_number();
    }
}

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

[[nodiscard]] bool is_numeric_column_kind(redis_column_kind kind) noexcept {
    return kind == redis_column_kind::signed_value ||
           kind == redis_column_kind::unsigned_value ||
           kind == redis_column_kind::float32 ||
           kind == redis_column_kind::float64;
}

void require_index_fits_column(redis_index_kind index_kind, redis_column_kind column_kind) {
    switch (index_kind) {
        case redis_index_kind::tag:
            if (column_kind != redis_column_kind::text && column_kind != redis_column_kind::bool_value) {
                throw std::invalid_argument("Redis TAG indexes require string or boolean columns");
            }
            return;
        case redis_index_kind::text:
            if (column_kind != redis_column_kind::text) {
                throw std::invalid_argument("Redis TEXT indexes require string columns");
            }
            return;
        case redis_index_kind::numeric:
            if (!is_numeric_column_kind(column_kind)) {
                throw std::invalid_argument("Redis NUMERIC indexes require numeric columns");
            }
            return;
        case redis_index_kind::none:
            break;
    }
    throw std::invalid_argument("Redis index kind must be TAG, TEXT or NUMERIC");
}

[[nodiscard]] std::pmr::string redis_index_name(std::string_view prefix, std::pmr::memory_resource* resource) {
    std::pmr::string result_value(prefix, resource);
    result_value += ":idx";
    return result_value;
}

// Shadow hash fields share the reserved __ruvia_ namespace with the entity
// marker. Index columns are safe identifiers (normalize_redis_mapping), and
// queries only name shadows of index columns; entity writes use the declared
// column names as they are.
[[nodiscard]] std::pmr::string shadow_field(
    std::string_view shadow, std::string_view field, std::pmr::memory_resource* resource) {
    std::pmr::string result_value(shadow, resource);
    result_value += field;
    return result_value;
}

[[nodiscard]] const redis_column_info& find_column_info(
    std::span<const redis_column_info> columns, std::string_view name) {
    const auto column = std::ranges::find(columns, name, &redis_column_info::name_);
    if (column == columns.end()) {
        throw std::invalid_argument("unknown Redis query column");
    }
    return *column;
}

void validate_entity_prefix(std::string_view entity_prefix, std::string_view table_value) {
    if (!table_value.empty() && table_value != entity_prefix) {
        throw std::invalid_argument("Redis predicate belongs to another entity prefix");
    }
}

// The text writes store for the same value, so a predicate matches stored data.
[[nodiscard]] std::pmr::string canonical_numeric(
    const redis_literal& value, redis_column_kind column_kind, std::pmr::memory_resource* resource) {
    switch (value.kind()) {
        case redis_literal_kind::signed_integer:
            require_redis_exact_number(static_cast<double>(value.signed_value()));
            return encode_redis_number(value.signed_value(), resource);
        case redis_literal_kind::unsigned_integer:
            require_redis_exact_number(static_cast<double>(value.unsigned_value()));
            return encode_redis_number(value.unsigned_value(), resource);
        case redis_literal_kind::real: {
            const double number = value.real_value();
            require_redis_exact_number(number);
            if (column_kind == redis_column_kind::float32) {
                return encode_redis_number(static_cast<float>(number), resource);
            }
            return encode_redis_number(number, resource);
        }
        case redis_literal_kind::boolean:
            return encode_redis_scalar(value.boolean_value(), resource);
        default:
            throw std::invalid_argument("Redis numeric predicate requires a numeric value");
    }
}

// TAG indexes only cover text and boolean columns.
[[nodiscard]] std::pmr::string canonical_tag(
    const redis_literal& value, redis_column_kind column_kind, std::pmr::memory_resource* resource) {
    if (value.kind() == redis_literal_kind::string && column_kind == redis_column_kind::text) {
        return encode_redis_tag(value.text(), resource);
    }
    if (value.kind() == redis_literal_kind::boolean && column_kind == redis_column_kind::bool_value) {
        return encode_redis_tag(value.boolean_value() ? "1" : "0", resource);
    }
    throw std::invalid_argument("Redis TAG predicate value type does not match column");
}

// Primary keys are text or integer columns (is_redis_entity_primary_key_scalar).
[[nodiscard]] std::pmr::string canonical_primary_key(
    const redis_literal& value, redis_column_kind column_kind, std::pmr::memory_resource* resource) {
    if (column_kind == redis_column_kind::text) {
        if (value.kind() != redis_literal_kind::string) {
            throw std::invalid_argument("Redis TEXT predicate requires a string value");
        }
        return std::pmr::string(value.text(), resource);
    }
    switch (value.kind()) {
        case redis_literal_kind::signed_integer:
            return encode_redis_number(value.signed_value(), resource);
        case redis_literal_kind::unsigned_integer:
            return encode_redis_number(value.unsigned_value(), resource);
        case redis_literal_kind::real: {
            const double number = value.real_value();
            if (!std::isfinite(number) || std::trunc(number) != number ||
                std::fabs(number) > redis_max_exact_integer) {
                throw std::invalid_argument("Redis integer primary key is invalid");
            }
            return encode_redis_number(static_cast<std::int64_t>(number), resource);
        }
        default:
            throw std::invalid_argument("Redis primary key requires a string or integer value");
    }
}

[[nodiscard]] redis_index_kind index_kind(const redis_mapping& mapping, std::string_view field) {
    const auto kind = mapping.index_kind(field);
    if (kind == redis_index_kind::none) {
        throw std::invalid_argument("Redis query column is not indexed");
    }
    return kind;
}

// The column of a comparison, IN or BETWEEN node and its first value operand:
// a literal, the IN list or the BETWEEN lower bound.
struct redis_column_comparison final {
    const redis_column_info& column_;
    const redis_predicate_node& operand_;
};

[[nodiscard]] redis_column_comparison column_comparison(const redis_mapping& mapping,
    std::span<const redis_predicate_node> nodes, const redis_predicate_node& node) {
    if (node.operands_.size() < 2U || nodes[node.operands_[0]].kind_ != node_kind::field) {
        throw std::logic_error("invalid Redis predicate expression");
    }
    const auto& field = nodes[node.operands_[0]];
    validate_entity_prefix(mapping.entity_prefix_, field.entity_prefix_);
    return {find_column_info(mapping.columns_, field.field_), nodes[node.operands_[1]]};
}

// `@field:{value}`
void append_tag_term(std::pmr::string& output, std::string_view field, std::string_view value) {
    output += '@';
    output += field;
    output += ":{";
    output += value;
    output += '}';
}

// `@field:[low high]`; a bound prefixed with '(' is exclusive.
void append_numeric_range(
    std::pmr::string& output, std::string_view field, std::string_view low, std::string_view high) {
    output += '@';
    output += field;
    output += ":[";
    output += low;
    output += ' ';
    output += high;
    output += ']';
}

void append_presence(std::pmr::string& output, std::string_view field, std::pmr::memory_resource* resource) {
    // Presence markers are written as the raw value 1. They are indexed as a
    // TAG shadow field, but are not passed through the user TAG encoding.
    append_tag_term(output, shadow_field(present_shadow, field, resource), present_value);
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
    if (value.kind() == redis_literal_kind::null) {
        append_null_comparison(output, field, operation, "unsupported NULL TAG comparison", resource);
        return;
    }
    const auto encoded = canonical_tag(value, column_kind, resource);
    switch (operation) {
        case redis_binary_operator::equal:
            append_tag_term(output, field, encoded);
            return;
        case redis_binary_operator::not_equal:
            output.push_back('(');
            append_presence(output, field, resource);
            output += " -";
            append_tag_term(output, field, encoded);
            output.push_back(')');
            return;
        default:
            throw std::invalid_argument("Redis TAG indexes only support equality predicates");
    }
}

void append_numeric_comparison(std::pmr::string& output, std::string_view field, const redis_literal& value,
    redis_column_kind column_kind, redis_binary_operator operation, std::pmr::memory_resource* resource) {
    if (value.kind() == redis_literal_kind::null) {
        append_null_comparison(output, field, operation, "unsupported NULL NUMERIC comparison", resource);
        return;
    }
    auto number = canonical_numeric(value, column_kind, resource);
    switch (operation) {
        case redis_binary_operator::equal:
            append_numeric_range(output, field, number, number);
            return;
        case redis_binary_operator::not_equal:
            output.push_back('(');
            append_numeric_range(output, field, "-inf", "+inf");
            output += " -";
            append_numeric_range(output, field, number, number);
            output.push_back(')');
            return;
        case redis_binary_operator::less:
            number.insert(number.begin(), '(');
            append_numeric_range(output, field, "-inf", number);
            return;
        case redis_binary_operator::less_equal:
            append_numeric_range(output, field, "-inf", number);
            return;
        case redis_binary_operator::greater:
            number.insert(number.begin(), '(');
            append_numeric_range(output, field, number, "+inf");
            return;
        case redis_binary_operator::greater_equal:
            append_numeric_range(output, field, number, "+inf");
            return;
        default:
            throw std::invalid_argument("unsupported Redis NUMERIC comparison");
    }
}

// Compiles one predicate tree against one repository mapping.
class redis_query_compiler final {
public:
    redis_query_compiler(const redis_mapping& mapping, std::pmr::memory_resource* resource) noexcept
        : mapping_(mapping),
          resource_(resource) {}

    void append_predicate(std::pmr::string& output, const redis_predicate_storage& predicate) const {
        append_expression(output, predicate.nodes_, predicate.nodes_[predicate.root_], 0U);
    }

    [[nodiscard]] std::pmr::string sort_field(std::string_view field) const {
        (void)find_column_info(mapping_.columns_, field);
        if (!mapping_.sortable(field)) {
            throw std::invalid_argument("Redis order column is not sortable");
        }
        if (mapping_.index_kind(field) == redis_index_kind::tag) {
            return shadow_field(sort_shadow, field, resource_);
        }
        return std::pmr::string(field, resource_);
    }

private:
    using nodes_type = std::span<const redis_predicate_node>;

    void append_expression(std::pmr::string& output, nodes_type nodes, const redis_predicate_node& node,
        std::size_t depth) const {
        constexpr std::size_t max_depth = 64U;
        if (depth > max_depth) {
            throw std::invalid_argument("Redis predicate nesting is too deep");
        }
        if (node.kind_ == node_kind::between) {
            append_between(output, nodes, node);
            return;
        }
        switch (node.binary_) {
            case redis_binary_operator::logical_and:
            case redis_binary_operator::logical_or:
                output.push_back('(');
                append_expression(output, nodes, nodes[node.operands_[0]], depth + 1U);
                output.push_back(node.binary_ == redis_binary_operator::logical_and ? ' ' : '|');
                append_expression(output, nodes, nodes[node.operands_[1]], depth + 1U);
                output.push_back(')');
                return;
            case redis_binary_operator::in:
            case redis_binary_operator::not_in:
                append_in_list(output, nodes, node);
                return;
            default:
                append_comparison(output, nodes, node);
                return;
        }
    }

    void append_comparison(std::pmr::string& output, nodes_type nodes, const redis_predicate_node& node) const {
        const auto comparison = column_comparison(mapping_, nodes, node);
        const auto field = comparison.column_.name_;
        const auto& value = comparison.operand_.literal_;
        switch (index_kind(mapping_, field)) {
            case redis_index_kind::tag:
                append_tag_comparison(output, field, value, comparison.column_.kind_, node.binary_, resource_);
                return;
            case redis_index_kind::numeric:
                append_numeric_comparison(output, field, value, comparison.column_.kind_, node.binary_, resource_);
                return;
            default:
                break;
        }
        // A TEXT index only answers presence tests.
        if (value.kind() == redis_literal_kind::null &&
            (node.binary_ == redis_binary_operator::equal || node.binary_ == redis_binary_operator::not_equal)) {
            append_null_comparison(output, field, node.binary_, {}, resource_);
            return;
        }
        throw std::invalid_argument("Redis TEXT predicates are not exposed by redis_predicate");
    }

    void append_in_list(std::pmr::string& output, nodes_type nodes, const redis_predicate_node& node) const {
        const auto comparison = column_comparison(mapping_, nodes, node);
        const auto field = comparison.column_.name_;
        const auto& items = comparison.operand_.operands_;
        const bool negated = node.binary_ == redis_binary_operator::not_in;
        if (items.empty() && negated) {
            output.push_back('*');
            return;
        }
        const auto kind = index_kind(mapping_, field);
        if (items.empty()) {
            // Presence markers only ever hold 1, so this matches nothing.
            append_tag_term(output, shadow_field(present_shadow, field, resource_), "x00");
            return;
        }
        if (kind == redis_index_kind::text) {
            throw std::invalid_argument("Redis TEXT predicates are not supported");
        }
        output.push_back('(');
        if (negated) {
            append_presence(output, field, resource_);
            output += " -(";
        }
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (index != 0U) {
                output.push_back('|');
            }
            const auto& value = nodes[items[index]].literal_;
            if (value.kind() == redis_literal_kind::null) {
                throw std::invalid_argument("Redis IN list requires non-null literal values");
            }
            if (kind == redis_index_kind::tag) {
                append_tag_term(output, field, canonical_tag(value, comparison.column_.kind_, resource_));
            } else {
                const auto number = canonical_numeric(value, comparison.column_.kind_, resource_);
                append_numeric_range(output, field, number, number);
            }
        }
        output.push_back(')');
        if (negated) {
            output.push_back(')');
        }
    }

    void append_between(std::pmr::string& output, nodes_type nodes, const redis_predicate_node& node) const {
        const auto comparison = column_comparison(mapping_, nodes, node);
        const auto field = comparison.column_.name_;
        const auto& lower = comparison.operand_.literal_;
        const auto& upper = nodes[node.operands_[2]].literal_;
        if (index_kind(mapping_, field) != redis_index_kind::numeric ||
            lower.kind() == redis_literal_kind::null || upper.kind() == redis_literal_kind::null) {
            throw std::invalid_argument("Redis BETWEEN requires a NUMERIC indexed column");
        }
        const auto low = canonical_numeric(lower, comparison.column_.kind_, resource_);
        const auto high = canonical_numeric(upper, comparison.column_.kind_, resource_);
        append_numeric_range(output, field, low, high);
    }

    const redis_mapping& mapping_;
    std::pmr::memory_resource* resource_;
};

// Written values are our own encodings of numeric columns, so this only
// enforces the exact range RediSearch can index.
void require_exact_numeric_text(std::string_view value) {
    double number = 0.0;
    const auto parsed_value = std::from_chars(value.data(), value.data() + value.size(), number,
        std::chars_format::general);
    if (parsed_value.ec != std::errc{} || parsed_value.ptr != value.data() + value.size()) {
        throw_inexact_redis_number();
    }
    require_redis_exact_number(number);
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

redis_mapping normalize_redis_mapping(std::span<const redis_column_info> columns, std::string_view entity_prefix,
    const redis_repository_config& config, std::pmr::memory_resource* resource) {
    const std::string_view prefix = config.prefix_.empty() ? entity_prefix : std::string_view(config.prefix_);
    if (prefix.empty()) {
        throw std::invalid_argument("Redis entity prefix cannot be empty");
    }
    if (prefix.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("invalid Redis entity prefix");
    }
    for (const auto& column : columns) {
        if (column.name_.starts_with("__ruvia_")) {
            throw std::invalid_argument("Redis entity column uses reserved __ruvia_ namespace");
        }
    }
    redis_mapping result_value{columns, entity_prefix, std::pmr::string(prefix, resource),
        std::pmr::vector<redis_index_definition>(resource)};
    result_value.indexes_.reserve(config.indexes_.size());
    for (const auto& index : config.indexes_) {
        const std::string_view field = index.field_;
        if (result_value.index_kind(field) != redis_index_kind::none) {
            throw std::invalid_argument("duplicate Redis index column");
        }
        const auto column = std::ranges::find(columns, field, &redis_column_info::name_);
        if (column == columns.end()) {
            throw std::invalid_argument("unknown Redis index column");
        }
        // Index columns name query fields and shadow hash fields.
        if (!is_safe_identifier(field)) {
            throw std::invalid_argument("invalid Redis index column name");
        }
        require_index_fits_column(index.kind_, column->kind_);
        result_value.indexes_.push_back({std::pmr::string(field, resource), index.kind_, index.sortable_});
    }
    return result_value;
}

std::pmr::string encode_redis_tag(std::string_view value, std::pmr::memory_resource* resource) {
    std::pmr::string encoded(pmr_resource_or_default(resource));
    encoded.reserve(value.size() * 2U + 1U);
    encoded.push_back('x');
    http_append_lower_hex(encoded, value);
    return encoded;
}

std::pmr::vector<std::pmr::string> compile_redis_find(const redis_mapping& mapping,
    const redis_find_options& options, std::pmr::memory_resource* resource, bool count_only,
    std::optional<std::uint64_t> take_override) {
    resource = pmr_resource_or_default(resource);
    const redis_query_compiler compiler(mapping, resource);
    std::pmr::vector<std::pmr::string> args(resource);
    emplace_redis_string(args, "FT.SEARCH");
    args.push_back(redis_index_name(mapping.prefix_, resource));
    std::pmr::string query(resource);
    if (const auto* predicate = redis_predicate_access::storage(options.where_)) {
        compiler.append_predicate(query, *predicate);
    } else {
        query = "*";
    }
    args.emplace_back(std::move(query));
    emplace_redis_string(args, "LIMIT");
    constexpr auto max_redis_integer = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto skip = options.skip_.value_or(0U);
    const auto requested_take = options.take_.value_or(redis_find_default_take);
    if (skip > max_redis_integer || requested_take > max_redis_integer) {
        throw std::invalid_argument("Redis Search LIMIT exceeds signed 64-bit range");
    }
    std::optional<std::pmr::string> sort_field_name;
    if (options.order_.has_value()) {
        const auto& order = *options.order_;
        sort_field_name = compiler.sort_field(order.field_);
        switch (order.direction_) {
            case redis_order_direction::asc:
            case redis_order_direction::desc:
                break;
            default:
                throw std::invalid_argument("invalid Redis order direction");
        }
    }
    if (count_only) {
        emplace_redis_string(args, "0");
        emplace_redis_string(args, "0");
    } else {
        const auto take = take_override.value_or(requested_take);
        if (take > max_redis_integer) {
            throw std::invalid_argument("Redis Search LIMIT exceeds signed 64-bit range");
        }
        std::pmr::string encoded_skip(resource), encoded_take(resource);
        http_append_unsigned(encoded_skip, skip);
        http_append_unsigned(encoded_take, take);
        args.emplace_back(std::move(encoded_skip));
        args.emplace_back(std::move(encoded_take));
    }
    if (!count_only && sort_field_name.has_value()) {
        emplace_redis_string(args, "SORTBY");
        args.push_back(std::move(*sort_field_name));
        emplace_redis_string(
            args, options.order_->direction_ == redis_order_direction::asc ? "ASC" : "DESC");
    }
    emplace_redis_string(args, "DIALECT");
    emplace_redis_string(args, "2");
    return args;
}

std::pmr::vector<std::pmr::string> compile_redis_index(
    const redis_mapping& mapping, std::pmr::memory_resource* resource) {
    // A mapping without indexes is valid for key-based access only.
    if (mapping.indexes_.empty()) {
        throw std::invalid_argument("Redis repository requires at least one index column");
    }
    resource = pmr_resource_or_default(resource);
    std::pmr::vector<std::pmr::string> args(resource);
    emplace_redis_string(args, "FT.CREATE");
    args.push_back(redis_index_name(mapping.prefix_, resource));
    emplace_redis_string(args, "ON");
    emplace_redis_string(args, "HASH");
    emplace_redis_string(args, "PREFIX");
    emplace_redis_string(args, "1");
    args.push_back(redis_entity_storage_prefix(mapping.prefix_, resource));
    emplace_redis_string(args, "SCHEMA");
    for (const auto& definition : mapping.indexes_) {
        const std::string_view field = definition.field_;
        if (definition.kind_ == redis_index_kind::tag) {
            args.push_back(shadow_field(tag_shadow, field, resource));
            emplace_redis_string(args, "AS");
            emplace_redis_string(args, field);
            emplace_redis_string(args, "TAG");
            emplace_redis_string(args, "CASESENSITIVE");
        } else {
            emplace_redis_string(args, field);
            emplace_redis_string(args, definition.kind_ == redis_index_kind::text ? "TEXT" : "NUMERIC");
            if (definition.sortable_) {
                emplace_redis_string(args, "SORTABLE");
            }
        }
        const auto presence = shadow_field(present_shadow, field, resource);
        emplace_redis_string(args, presence);
        emplace_redis_string(args, "AS");
        emplace_redis_string(args, presence);
        emplace_redis_string(args, "TAG");
        emplace_redis_string(args, "CASESENSITIVE");
        if (definition.kind_ == redis_index_kind::tag && definition.sortable_) {
            emplace_redis_string(args, field);
            emplace_redis_string(args, "AS");
            args.push_back(shadow_field(sort_shadow, field, resource));
            emplace_redis_string(args, "TAG");
            emplace_redis_string(args, "SORTABLE");
            emplace_redis_string(args, "NOINDEX");
        }
    }
    return args;
}

std::optional<std::pmr::string> redis_primary_key(
    const redis_mapping& mapping, const redis_predicate& predicate, std::pmr::memory_resource* resource) {
    resource = pmr_resource_or_default(resource);
    const auto* storage = redis_predicate_access::storage(predicate);
    if (storage == nullptr) {
        return std::nullopt;
    }
    const auto& root = storage->nodes_[storage->root_];
    if (root.kind_ != node_kind::binary || root.binary_ != redis_binary_operator::equal) {
        return std::nullopt;
    }
    const auto comparison = column_comparison(mapping, storage->nodes_, root);
    if (!comparison.column_.primary_key_) {
        return std::nullopt;
    }
    const auto& value = comparison.operand_.literal_;
    if (value.kind() == redis_literal_kind::null) {
        throw std::invalid_argument("Redis primary key cannot be NULL");
    }
    auto result_value = canonical_primary_key(value, comparison.column_.kind_, resource);
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
        require_exact_numeric_text(*value);
    }
    append_set(args, field, *value);
    append_set(args, presence, present_value);
    if (tag_shadow_value) {
        append_set(args, shadow_field(tag_shadow, field, resource), encode_redis_tag(*value, resource));
    }
}

}  // namespace ruvia::detail
