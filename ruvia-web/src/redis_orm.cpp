#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include "connection_lifecycle.h"
#include "http_client_pool.h"
#include "http_header.h"
#include "redis.h"
#include "redis_pool.h"

namespace ruvia::detail {
namespace {

// ARGV: mode, ttl policy, milliseconds, required count, required names,
// followed by mutation triples (set/delete, field, value). All validation
// precedes writes: Redis scripts are atomic but do not roll back runtime errors.
constexpr std::string_view write_script = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
local exists = kind ~= 'none'
if exists and kind ~= 'hash' then return redis.error_reply('WRONGTYPE entity key is not a hash') end
if exists and redis.call('HGET', KEYS[1], '__ruvia_entity') ~= '1' then
    return redis.error_reply('entity key is not owned by this repository')
end
local required = tonumber(ARGV[4])
local start = 5 + required
local changes = {}
for i = start, #ARGV, 3 do changes[ARGV[i + 1]] = ARGV[i] end
for i = 5, 4 + required do
    local field = ARGV[i]
    if changes[field] == 'd' or
       (changes[field] ~= 's' and (ARGV[1] == 'insert' or (ARGV[1] ~= 'update' and not exists) or (exists and redis.call('HEXISTS', KEYS[1], field) == 0))) then
        return redis.error_reply('required entity field is missing: ' .. field)
    end
end
if ARGV[1] == 'insert' and exists then return 0 end
if ARGV[1] == 'update' and not exists then return 0 end
for i = start, #ARGV, 3 do
    if ARGV[i] == 's' then redis.call('HSET', KEYS[1], ARGV[i + 1], ARGV[i + 2])
    else redis.call('HDEL', KEYS[1], ARGV[i + 1]) end
end
redis.call('HSET', KEYS[1], '__ruvia_entity', '1')
if ARGV[2] == 'expire' then redis.call('PEXPIRE', KEYS[1], ARGV[3])
elseif ARGV[2] == 'persist' then redis.call('PERSIST', KEYS[1]) end
if exists then return 2 else return 1 end
)lua";

constexpr std::string_view delete_script = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
if kind == 'none' then return 0 end
if kind ~= 'hash' then return redis.error_reply('WRONGTYPE entity key is not a hash') end
if redis.call('HGET', KEYS[1], '__ruvia_entity') ~= '1' then
    return redis.error_reply('entity key is not owned by this repository')
end
return redis.call('DEL', KEYS[1])
)lua";

// Owns the digest text the script view borrows; built once per script.
class redis_orm_script_digest final {
public:
    explicit redis_orm_script_digest(std::string_view body) {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        std::size_t digest_size = 0;
        if (EVP_Q_digest(nullptr, "SHA1", nullptr, body.data(), body.size(), digest.data(), &digest_size) != 1 || digest_size * 2 != sha1_.size()) {
            throw std::runtime_error("redis entity script digest could not be computed");
        }
        http_encode_lower_hex(sha1_.data(), std::span<const unsigned char>(digest.data(), digest_size));
        script_ = {body, std::string_view(sha1_.data(), sha1_.size())};
    }

    [[nodiscard]] const redis_orm_script& script() const noexcept {
        return script_;
    }

private:
    std::array<char, 40> sha1_{};
    redis_orm_script script_;
};

void append_redis_orm_script_call(redis_orm_arguments_type& args, const redis_orm_script& script) {
    args.emplace_back("EVALSHA");
    args.emplace_back(script.sha1_);
    args.emplace_back("1");
}

}  // namespace

const redis_orm_script& redis_orm_write_script() {
    static const redis_orm_script_digest digest(write_script);
    return digest.script();
}

const redis_orm_script& redis_orm_delete_script() {
    static const redis_orm_script_digest digest(delete_script);
    return digest.script();
}

redis_orm_arguments_type redis_orm_write_arguments(std::string_view storage_prefix, std::string_view id, std::string_view mode, redis_write_options options, std::size_t extra_arguments) {
    if (options.ttl_ && (options.persist_ || options.ttl_->count() <= 0 || options.ttl_->count() > 9007199254740991LL)) {
        throw std::invalid_argument("entity TTL must be positive, at most 2^53-1 milliseconds, and cannot combine with persist");
    }
    redis_orm_arguments_type args;
    args.reserve(7 + extra_arguments);
    append_redis_orm_script_call(args, redis_orm_write_script());
    args.push_back(redis_entity_key(storage_prefix, id));
    args.emplace_back(mode);
    args.emplace_back(options.ttl_ ? "expire" : (options.persist_ ? "persist" : "keep"));
    if (options.ttl_) {
        http_append_signed(args.emplace_back(), static_cast<std::int64_t>(options.ttl_->count()));
    } else {
        args.emplace_back("0");
    }
    return args;
}

redis_orm_arguments_type redis_orm_delete_arguments(std::string_view storage_prefix, std::string_view id) {
    redis_orm_arguments_type args;
    args.reserve(4);
    append_redis_orm_script_call(args, redis_orm_delete_script());
    args.push_back(redis_entity_key(storage_prefix, id));
    return args;
}

task<redis_value> execute_redis_orm_script(redis_pool& pool, const redis_orm_script& script, redis_orm_arguments_type args, request_io_metrics* io_metrics, operation_options options) {
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    const operation_deadline deadline(options.timeout_);
    {
        auto reply = co_await pool.execute_borrowed(redis_command_args_view{args}, options, deadline);
        if (reply.kind() != redis_value::kind_type::error || !reply.error().starts_with("NOSCRIPT")) {
            co_return reply;
        }
        // The error is consumed by this decision; release its storage before
        // the fallback waits for its connection and response.
    }
    // The server's script cache was flushed or never loaded this script:
    // EVAL runs the body and caches it under the same digest for later calls.
    args[0].assign("EVAL");
    args[1].assign(script.body_);
    co_return co_await pool.execute_owned(std::move(args), std::move(options), std::nullopt, nullptr, deadline);
}

redis_write_result redis_orm_exec_result(redis_value&& reply) {
    const auto value = redis_value_integer(reply);
    switch (value) {
        case 0:
            return redis_write_result(0);
        case 1:
        case 2:
            return redis_write_result(1);
        default:
            throw redis_error(redis_error::code_type::protocol_error, "invalid entity write affected-row reply");
    }
}

redis_write_result redis_orm_delete_result(redis_value&& reply) {
    const auto value = redis_value_integer(reply);
    if (value == 0 || value == 1) {
        return redis_write_result(static_cast<std::uint64_t>(value));
    }
    throw redis_error(redis_error::code_type::protocol_error, "invalid entity delete affected-row reply");
}

void redis_orm_status_result(redis_value&& reply) {
    if (redis_value_string(reply) != "OK") {
        throw redis_error(redis_error::code_type::protocol_error, "invalid entity index status reply");
    }
}

std::uint64_t redis_orm_count(const redis_value& reply) {
    const auto count = redis_value_integer(reply);
    if (count < 0) {
        throw redis_error(redis_error::code_type::protocol_error, "negative entity result count");
    }
    return static_cast<std::uint64_t>(count);
}

std::uint64_t redis_orm_count_result(redis_value&& reply) {
    const auto values = redis_value_array(reply);
    if (values.size() != 1) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid entity count reply");
    }
    return redis_orm_count(values.front());
}

bool redis_orm_exists_result(redis_value&& reply) {
    return redis_orm_count_result(std::move(reply)) != 0;
}

}  // namespace ruvia::detail

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
    throw std::invalid_argument("numeric search values must be finite and within the exact integer range of Redis Search");
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
        if (!(ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_numeric_column_kind(redis_column_kind kind) noexcept {
    return kind == redis_column_kind::signed_value || kind == redis_column_kind::unsigned_value || kind == redis_column_kind::float32 || kind == redis_column_kind::float64;
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

[[nodiscard]] std::string redis_index_name(std::string_view prefix) {
    std::string result_value;
    result_value.reserve(prefix.size() + 4U);
    result_value.append(prefix);
    result_value.append(":idx");
    return result_value;
}

// Shadow hash fields share the reserved __ruvia_ namespace with the entity
// marker. Index columns are safe identifiers (normalize_redis_mapping), and
// queries only name shadows of index columns; entity writes use the declared
// column names as they are.
[[nodiscard]] std::string shadow_field(std::string_view shadow, std::string_view field) {
    std::string result_value;
    result_value.reserve(shadow.size() + field.size());
    result_value.append(shadow);
    result_value.append(field);
    return result_value;
}

[[nodiscard]] const redis_column_info& find_column_info(std::span<const redis_column_info> columns, std::string_view name) {
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

// Bound text shares encode_redis_number with entity writes and never narrows a
// literal. A float operand keeps float's encoding for a float32 column and is
// widened for other numeric columns; double operands always keep their width.
// The optional exclusive marker '(' precedes the number in the same buffer.
using redis_numeric_bound_buffer = std::array<char, redis_scalar_scratch_size + 1>;

[[nodiscard]] std::string_view format_canonical_numeric(redis_numeric_bound_buffer& buffer, const redis_literal& value, redis_column_kind column_kind, bool exclusive = false) {
    const std::span<char, redis_scalar_scratch_size> scratch(buffer.data() + 1, redis_scalar_scratch_size);
    std::string_view number;
    switch (value.kind()) {
        case redis_literal_kind::signed_integer:
            require_redis_exact_number(static_cast<double>(value.signed_value()));
            number = encode_redis_number(value.signed_value(), scratch);
            break;
        case redis_literal_kind::unsigned_integer:
            require_redis_exact_number(static_cast<double>(value.unsigned_value()));
            number = encode_redis_number(value.unsigned_value(), scratch);
            break;
        case redis_literal_kind::float32: {
            const float real = value.float32_value();
            require_redis_exact_number(real);
            number = column_kind == redis_column_kind::float32 ? encode_redis_number(real, scratch) : encode_redis_number(static_cast<double>(real), scratch);
            break;
        }
        case redis_literal_kind::real: {
            const double real = value.real_value();
            require_redis_exact_number(real);
            number = encode_redis_number(real, scratch);
            break;
        }
        default:
            throw std::invalid_argument("Redis numeric predicate requires a numeric value");
    }
    if (!exclusive) {
        return number;
    }
    // The number lies inside scratch, which starts one byte into the buffer.
    const auto offset = static_cast<std::size_t>(number.data() - buffer.data());
    buffer[offset - 1] = '(';
    return std::string_view(buffer.data() + offset - 1, number.size() + 1);
}

// TAG indexes only cover text and boolean columns.
[[nodiscard]] std::string_view raw_tag_value(const redis_literal& value, redis_column_kind column_kind) {
    if (value.kind() == redis_literal_kind::string && column_kind == redis_column_kind::text) {
        return value.text();
    }
    if (value.kind() == redis_literal_kind::boolean && column_kind == redis_column_kind::bool_value) {
        return value.boolean_value() ? std::string_view("1") : std::string_view("0");
    }
    throw std::invalid_argument("Redis TAG predicate value type does not match column");
}

// Primary keys are text or integer columns (is_redis_entity_primary_key_scalar).
[[nodiscard]] std::string_view canonical_primary_key(const redis_literal& value, redis_column_kind column_kind, std::span<char, redis_scalar_scratch_size> scratch) {
    if (column_kind == redis_column_kind::text) {
        if (value.kind() != redis_literal_kind::string) {
            throw std::invalid_argument("Redis TEXT predicate requires a string value");
        }
        return value.text();
    }
    constexpr auto max_signed = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    switch (value.kind()) {
        case redis_literal_kind::signed_integer:
            if (column_kind == redis_column_kind::unsigned_value && value.signed_value() < 0) {
                throw std::invalid_argument("Redis integer primary key is invalid");
            }
            return encode_redis_number(value.signed_value(), scratch);
        case redis_literal_kind::unsigned_integer:
            if (column_kind == redis_column_kind::signed_value && value.unsigned_value() > max_signed) {
                throw std::invalid_argument("Redis integer primary key is invalid");
            }
            return encode_redis_number(value.unsigned_value(), scratch);
        case redis_literal_kind::float32:
        case redis_literal_kind::real: {
            const double number = value.kind() == redis_literal_kind::float32 ? static_cast<double>(value.float32_value()) : value.real_value();
            if (!std::isfinite(number) || std::trunc(number) != number || std::fabs(number) > redis_max_exact_integer || (column_kind == redis_column_kind::unsigned_value && number < 0.0)) {
                throw std::invalid_argument("Redis integer primary key is invalid");
            }
            return encode_redis_number(static_cast<std::int64_t>(number), scratch);
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

[[nodiscard]] redis_column_comparison column_comparison(const redis_mapping& mapping, std::span<const redis_predicate_node> nodes, const redis_predicate_node& node) {
    if (node.operands_.size() < 2U || nodes[node.operands_[0]].kind_ != node_kind::field) {
        throw std::logic_error("invalid Redis predicate expression");
    }
    const auto& field = nodes[node.operands_[0]];
    validate_entity_prefix(mapping.entity_prefix_, field.entity_prefix_);
    return {find_column_info(mapping.columns_, field.field_), nodes[node.operands_[1]]};
}

// `@shadow_field:{value}`
void append_shadow_tag_term(std::string& output, std::string_view shadow, std::string_view field, std::string_view value) {
    output.reserve(output.size() + 4U + shadow.size() + field.size() + value.size());
    output.push_back('@');
    output.append(shadow);
    output.append(field);
    output.append(":{");
    output.append(value);
    output.push_back('}');
}

// `@field:{x<hex>}`
void append_encoded_tag_term(std::string& output, std::string_view field, std::string_view raw_value) {
    output.reserve(output.size() + 5U + field.size() + raw_value.size() * 2U);
    output.push_back('@');
    output.append(field);
    output.append(":{x");
    http_append_lower_hex(output, raw_value);
    output.push_back('}');
}

// `@field:[low high]`; a bound prefixed with '(' is exclusive.
void append_numeric_range(std::string& output, std::string_view field, std::string_view low, std::string_view high) {
    output.reserve(output.size() + 5U + field.size() + low.size() + high.size());
    output.push_back('@');
    output.append(field);
    output.append(":[");
    output.append(low);
    output.push_back(' ');
    output.append(high);
    output.push_back(']');
}

void append_presence(std::string& output, std::string_view field) {
    // Presence markers are written as the raw value 1. They are indexed as a
    // TAG shadow field, but are not passed through the user TAG encoding.
    append_shadow_tag_term(output, present_shadow, field, present_value);
}

// `field IS NULL` / `field IS NOT NULL` through the presence shadow.
void append_null_comparison(std::string& output, std::string_view field, redis_binary_operator operation, const char* unsupported) {
    if (operation == redis_binary_operator::equal) {
        output.push_back('-');
        append_presence(output, field);
    } else if (operation == redis_binary_operator::not_equal) {
        append_presence(output, field);
    } else {
        throw std::invalid_argument(unsupported);
    }
}

void append_tag_comparison(std::string& output, std::string_view field, const redis_literal& value, redis_column_kind column_kind, redis_binary_operator operation) {
    if (value.kind() == redis_literal_kind::null) {
        append_null_comparison(output, field, operation, "unsupported NULL TAG comparison");
        return;
    }
    const auto raw = raw_tag_value(value, column_kind);
    switch (operation) {
        case redis_binary_operator::equal:
            append_encoded_tag_term(output, field, raw);
            return;
        case redis_binary_operator::not_equal:
            output.reserve(output.size() + 14U + present_shadow.size() + field.size() * 2U + raw.size() * 2U);
            output.push_back('(');
            append_presence(output, field);
            output.append(" -");
            append_encoded_tag_term(output, field, raw);
            output.push_back(')');
            return;
        default:
            throw std::invalid_argument("Redis TAG indexes only support equality predicates");
    }
}

void append_numeric_comparison(std::string& output, std::string_view field, const redis_literal& value, redis_column_kind column_kind, redis_binary_operator operation) {
    if (value.kind() == redis_literal_kind::null) {
        append_null_comparison(output, field, operation, "unsupported NULL NUMERIC comparison");
        return;
    }
    redis_numeric_bound_buffer number_buffer;
    switch (operation) {
        case redis_binary_operator::equal: {
            const auto number = format_canonical_numeric(number_buffer, value, column_kind);
            append_numeric_range(output, field, number, number);
            return;
        }
        case redis_binary_operator::not_equal: {
            const auto number = format_canonical_numeric(number_buffer, value, column_kind);
            output.reserve(output.size() + 23U + field.size() * 2U + number.size() * 2U);
            output.push_back('(');
            append_numeric_range(output, field, "-inf", "+inf");
            output.append(" -");
            append_numeric_range(output, field, number, number);
            output.push_back(')');
            return;
        }
        case redis_binary_operator::less: {
            const auto number = format_canonical_numeric(number_buffer, value, column_kind, true);
            append_numeric_range(output, field, "-inf", number);
            return;
        }
        case redis_binary_operator::less_equal: {
            const auto number = format_canonical_numeric(number_buffer, value, column_kind);
            append_numeric_range(output, field, "-inf", number);
            return;
        }
        case redis_binary_operator::greater: {
            const auto number = format_canonical_numeric(number_buffer, value, column_kind, true);
            append_numeric_range(output, field, number, "+inf");
            return;
        }
        case redis_binary_operator::greater_equal: {
            const auto number = format_canonical_numeric(number_buffer, value, column_kind);
            append_numeric_range(output, field, number, "+inf");
            return;
        }
        default:
            throw std::invalid_argument("unsupported Redis NUMERIC comparison");
    }
}

// Compiles one predicate tree against one repository mapping.
class redis_query_compiler final {
public:
    redis_query_compiler(const redis_mapping& mapping) noexcept :
        mapping_(mapping) {}

    void append_predicate(std::string& output, const redis_predicate_storage& predicate) const {
        append_expression(output, predicate.nodes_, predicate.nodes_[predicate.root_], 0U);
    }

    void validate_sort_field(std::string_view field) const {
        (void)find_column_info(mapping_.columns_, field);
        if (!mapping_.sortable(field)) {
            throw std::invalid_argument("Redis order column is not sortable");
        }
    }

    [[nodiscard]] std::string sort_field(std::string_view field) const {
        validate_sort_field(field);
        if (mapping_.index_kind(field) == redis_index_kind::tag) {
            return shadow_field(sort_shadow, field);
        }
        return std::string(field);
    }

private:
    using nodes_type = std::span<const redis_predicate_node>;

    void append_expression(std::string& output, nodes_type nodes, const redis_predicate_node& node, std::size_t depth) const {
        constexpr std::size_t max_depth = 64U;
        if (depth > max_depth) {
            throw std::invalid_argument("Redis predicate nesting is too deep");
        }
        if (node.kind_ == node_kind::between) {
            append_between(output, nodes, node);
            return;
        }
        if (node.kind_ == node_kind::logical_not) {
            if (node.operands_.empty()) {
                throw std::logic_error("invalid Redis predicate expression");
            }
            output.append("-(");
            append_expression(output, nodes, nodes[node.operands_[0]], depth + 1U);
            output.push_back(')');
            return;
        }
        switch (node.binary_) {
            case redis_binary_operator::logical_and:
            case redis_binary_operator::logical_or:
                if (node.operands_.size() < 2U) {
                    throw std::logic_error("invalid Redis predicate expression");
                }
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

    void append_comparison(std::string& output, nodes_type nodes, const redis_predicate_node& node) const {
        const auto comparison = column_comparison(mapping_, nodes, node);
        const auto field = comparison.column_.name_;
        const auto& value = comparison.operand_.literal_;
        if (comparison.column_.primary_key_ && comparison.column_.kind_ == redis_column_kind::text && value.kind() == redis_literal_kind::string && value.text().empty()) {
            throw std::invalid_argument("Redis primary key cannot be empty");
        }
        switch (index_kind(mapping_, field)) {
            case redis_index_kind::tag:
                append_tag_comparison(output, field, value, comparison.column_.kind_, node.binary_);
                return;
            case redis_index_kind::numeric:
                append_numeric_comparison(output, field, value, comparison.column_.kind_, node.binary_);
                return;
            default:
                break;
        }
        // A TEXT index only answers presence tests.
        if (value.kind() == redis_literal_kind::null && (node.binary_ == redis_binary_operator::equal || node.binary_ == redis_binary_operator::not_equal)) {
            append_null_comparison(output, field, node.binary_, {});
            return;
        }
        throw std::invalid_argument("Redis TEXT predicates are not exposed by redis_predicate");
    }

    void append_in_list(std::string& output, nodes_type nodes, const redis_predicate_node& node) const {
        const auto comparison = column_comparison(mapping_, nodes, node);
        const auto field = comparison.column_.name_;
        const auto& items = comparison.operand_.operands_;
        const bool negated = node.binary_ == redis_binary_operator::not_in;
        const auto kind = index_kind(mapping_, field);
        if (kind == redis_index_kind::text) {
            throw std::invalid_argument("Redis TEXT predicates are not supported");
        }
        if (items.empty() && negated) {
            append_presence(output, field);
            return;
        }
        if (items.empty()) {
            // Presence markers only ever hold 1, so this matches nothing.
            append_shadow_tag_term(output, present_shadow, field, "x00");
            return;
        }
        output.push_back('(');
        if (negated) {
            append_presence(output, field);
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
            if (comparison.column_.primary_key_ && comparison.column_.kind_ == redis_column_kind::text && value.kind() == redis_literal_kind::string && value.text().empty()) {
                throw std::invalid_argument("Redis primary key cannot be empty");
            }
            if (kind == redis_index_kind::tag) {
                append_encoded_tag_term(output, field, raw_tag_value(value, comparison.column_.kind_));
            } else {
                redis_numeric_bound_buffer number_buffer;
                const auto number = format_canonical_numeric(number_buffer, value, comparison.column_.kind_);
                append_numeric_range(output, field, number, number);
            }
        }
        output.push_back(')');
        if (negated) {
            output.push_back(')');
        }
    }

    void append_between(std::string& output, nodes_type nodes, const redis_predicate_node& node) const {
        const auto comparison = column_comparison(mapping_, nodes, node);
        if (node.operands_.size() < 3U) {
            throw std::logic_error("invalid Redis predicate expression");
        }
        const auto field = comparison.column_.name_;
        const auto& lower = comparison.operand_.literal_;
        const auto& upper = nodes[node.operands_[2]].literal_;
        if (index_kind(mapping_, field) != redis_index_kind::numeric || lower.kind() == redis_literal_kind::null || upper.kind() == redis_literal_kind::null) {
            throw std::invalid_argument("Redis BETWEEN requires a NUMERIC indexed column");
        }
        redis_numeric_bound_buffer low_buffer;
        redis_numeric_bound_buffer high_buffer;
        const auto low = format_canonical_numeric(low_buffer, lower, comparison.column_.kind_);
        const auto high = format_canonical_numeric(high_buffer, upper, comparison.column_.kind_);
        append_numeric_range(output, field, low, high);
    }

    const redis_mapping& mapping_;
};

// Written values are our own encodings of numeric columns, so this only
// enforces the exact range RediSearch can index.
void require_exact_numeric_text(std::string_view value) {
    double number = 0.0;
    const auto parsed_value = std::from_chars(value.data(), value.data() + value.size(), number, std::chars_format::general);
    if (parsed_value.ec != std::errc{} || parsed_value.ptr != value.data() + value.size()) {
        throw_inexact_redis_number();
    }
    require_redis_exact_number(number);
}

template <typename field_type, typename value_type>
void append_set(redis_orm_arguments_type& args, field_type&& field, value_type&& value) {
    args.emplace_back("s");
    args.emplace_back(std::forward<field_type>(field));
    args.emplace_back(std::forward<value_type>(value));
}

template <typename field_type>
void append_delete(redis_orm_arguments_type& args, field_type&& field) {
    args.emplace_back("d");
    args.emplace_back(std::forward<field_type>(field));
    args.emplace_back();
}

}  // namespace

redis_mapping normalize_redis_mapping(std::span<const redis_column_info> columns, std::string_view entity_prefix, const redis_repository_config& config) {
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
    redis_mapping result_value{columns, std::string(entity_prefix), std::string(prefix), redis_entity_storage_prefix(prefix), std::vector<redis_index_definition>{}, std::vector<redis_index_kind>(columns.size(), redis_index_kind::none)};
    result_value.indexes_.reserve(config.indexes_.size());
    for (const auto& index : config.indexes_) {
        const std::string_view field = index.column_;
        const auto column = std::ranges::find(columns, field, &redis_column_info::name_);
        if (column == columns.end()) {
            throw std::invalid_argument("unknown Redis index column");
        }
        auto& column_index_kind = result_value.column_index_kinds_[static_cast<std::size_t>(column - columns.begin())];
        if (column_index_kind != redis_index_kind::none) {
            throw std::invalid_argument("duplicate Redis index column");
        }
        // Index columns name query fields and shadow hash fields.
        if (!is_safe_identifier(field)) {
            throw std::invalid_argument("invalid Redis index column name");
        }
        require_index_fits_column(index.kind_, column->kind_);
        result_value.indexes_.push_back({std::string(field), index.kind_, index.sortable_});
        column_index_kind = index.kind_;
    }
    return result_value;
}

std::string encode_redis_tag(std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size() * 2U + 1U);
    encoded.push_back('x');
    http_append_lower_hex(encoded, value);
    return encoded;
}

}  // namespace ruvia::detail

namespace ruvia {

redis_search_mapping::redis_search_mapping(const redis_search_mapping& other) :
    mapping_{other.mapping_.columns_, std::string(other.mapping_.entity_prefix_), std::string(other.mapping_.prefix_), std::string(other.mapping_.storage_prefix_), std::vector<detail::redis_index_definition>{}, std::vector<redis_index_kind>(other.mapping_.column_index_kinds_)} {
    mapping_.indexes_.reserve(other.mapping_.indexes_.size());
    for (const auto& index : other.mapping_.indexes_) {
        mapping_.indexes_.push_back({std::string(index.column_), index.kind_, index.sortable_});
    }
}

redis_search_mapping& redis_search_mapping::operator=(const redis_search_mapping& other) {
    if (this != &other) {
        redis_search_mapping replacement(other);
        swap(replacement);
    }
    return *this;
}

redis_search_mapping& redis_search_mapping::operator=(redis_search_mapping&& other) {
    if (this != &other) {
        swap(other);
    }
    return *this;
}

void redis_search_mapping::swap(redis_search_mapping& other) noexcept {
    std::swap(mapping_.columns_, other.mapping_.columns_);
    mapping_.entity_prefix_.swap(other.mapping_.entity_prefix_);
    mapping_.prefix_.swap(other.mapping_.prefix_);
    mapping_.storage_prefix_.swap(other.mapping_.storage_prefix_);
    mapping_.indexes_.swap(other.mapping_.indexes_);
    mapping_.column_index_kinds_.swap(other.mapping_.column_index_kinds_);
}

void redis_search_mapping::require_valid() const {
    if (mapping_.prefix_.empty()) {
        throw std::logic_error("Redis search mapping has been moved from");
    }
}

redis_compiled_command redis_search_mapping::compile_find(const redis_find_options& options, redis_search_mode mode) const {
    require_valid();
    const auto& mapping = mapping_;
    const detail::redis_query_compiler compiler(mapping);
    const auto skip = options.skip_ ? options.skip_->value() : 0U;
    const auto requested_take = options.take_ ? options.take_->value() : redis_find_default_take;
    if (!std::in_range<std::int64_t>(skip) || !std::in_range<std::int64_t>(requested_take)) {
        throw std::invalid_argument("Redis search skip and take must fit in 64-bit signed range");
    }
    std::uint64_t take = requested_take;
    bool count_only = false;
    switch (mode) {
        case redis_search_mode::rows:
            break;
        case redis_search_mode::first:
            take = (std::min)(take, std::uint64_t{1});
            break;
        case redis_search_mode::count:
            count_only = true;
            break;
        default:
            throw std::invalid_argument("invalid Redis search mode");
    }
    std::optional<std::string> sort_field_name;
    if (options.order_.has_value()) {
        const auto& order = *options.order_;
        if (count_only) {
            compiler.validate_sort_field(order.column_);
        } else {
            sort_field_name = compiler.sort_field(order.column_);
        }
        switch (order.direction_) {
            case redis_order_direction::asc:
            case redis_order_direction::desc:
                break;
            default:
                throw std::invalid_argument("invalid Redis order direction");
        }
    }
    std::string query;
    if (const auto* predicate = detail::redis_predicate_access::storage(options.where_)) {
        query.reserve(predicate->nodes_.size() * 24U);
        compiler.append_predicate(query, *predicate);
    } else {
        query = "*";
    }
    std::vector<std::string> args;
    args.reserve(sort_field_name.has_value() ? 11U : 8U);
    args.emplace_back("FT.SEARCH");
    args.push_back(detail::redis_index_name(mapping.prefix_));
    args.emplace_back(std::move(query));
    args.emplace_back("LIMIT");
    if (count_only) {
        args.emplace_back("0");
        args.emplace_back("0");
    } else {
        std::string encoded_skip, encoded_take;
        http_append_unsigned(encoded_skip, skip);
        http_append_unsigned(encoded_take, take);
        args.emplace_back(std::move(encoded_skip));
        args.emplace_back(std::move(encoded_take));
    }
    if (sort_field_name.has_value()) {
        args.emplace_back("SORTBY");
        args.push_back(std::move(*sort_field_name));
        args.emplace_back(options.order_->direction_ == redis_order_direction::asc ? "ASC" : "DESC");
    }
    args.emplace_back("DIALECT");
    args.emplace_back("2");
    return redis_compiled_command(std::move(args));
}

redis_compiled_command redis_search_mapping::compile_create_index() const {
    require_valid();
    const auto& mapping = mapping_;
    // A mapping without indexes is valid for key-based access only.
    if (mapping.indexes_.empty()) {
        throw std::invalid_argument("Redis repository requires at least one index column");
    }
    std::size_t arg_count = 8U;
    for (const auto& definition : mapping.indexes_) {
        if (definition.kind_ == redis_index_kind::tag) {
            arg_count += definition.sortable_ ? 16U : 10U;
        } else {
            arg_count += definition.sortable_ ? 8U : 7U;
        }
    }
    std::vector<std::string> args;
    args.reserve(arg_count);
    args.emplace_back("FT.CREATE");
    args.push_back(detail::redis_index_name(mapping.prefix_));
    args.emplace_back("ON");
    args.emplace_back("HASH");
    args.emplace_back("PREFIX");
    args.emplace_back("1");
    args.emplace_back(mapping.storage_prefix_);
    args.emplace_back("SCHEMA");
    for (const auto& definition : mapping.indexes_) {
        const std::string_view field = definition.column_;
        if (definition.kind_ == redis_index_kind::tag) {
            args.push_back(detail::shadow_field(detail::tag_shadow, field));
            args.emplace_back("AS");
            args.emplace_back(field);
            args.emplace_back("TAG");
            args.emplace_back("CASESENSITIVE");
        } else {
            args.emplace_back(field);
            args.emplace_back(definition.kind_ == redis_index_kind::text ? "TEXT" : "NUMERIC");
            if (definition.sortable_) {
                args.emplace_back("SORTABLE");
            }
        }
        auto presence = detail::shadow_field(detail::present_shadow, field);
        args.emplace_back(presence);
        args.emplace_back("AS");
        args.push_back(std::move(presence));
        args.emplace_back("TAG");
        args.emplace_back("CASESENSITIVE");
        if (definition.kind_ == redis_index_kind::tag && definition.sortable_) {
            args.emplace_back(field);
            args.emplace_back("AS");
            args.push_back(detail::shadow_field(detail::sort_shadow, field));
            args.emplace_back("TAG");
            args.emplace_back("SORTABLE");
            args.emplace_back("NOINDEX");
        }
    }
    return redis_compiled_command(std::move(args));
}

redis_compiled_command redis_search_mapping::compile_drop_index() const {
    require_valid();
    std::vector<std::string> args;
    args.reserve(2U);
    args.emplace_back("FT.DROPINDEX");
    args.push_back(detail::redis_index_name(mapping_.prefix_));
    return redis_compiled_command(std::move(args));
}

}  // namespace ruvia

namespace ruvia::detail {

std::optional<std::string_view> redis_primary_key(const redis_mapping& mapping, const redis_predicate& predicate, std::span<char, redis_scalar_scratch_size> scratch) {
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
    const auto result_value = canonical_primary_key(value, comparison.column_.kind_, scratch);
    if (result_value.empty()) {
        throw std::invalid_argument("Redis primary key cannot be empty");
    }
    return result_value;
}

void append_redis_entity_field(redis_orm_arguments_type& args, std::string_view field, std::optional<std::string_view> value, bool numeric_index, bool tag_shadow_value) {
    if (!value) {
        append_delete(args, field);
        append_delete(args, shadow_field(present_shadow, field));
        if (tag_shadow_value) {
            append_delete(args, shadow_field(tag_shadow, field));
        }
        return;
    }
    if (numeric_index) {
        require_exact_numeric_text(*value);
    }
    append_set(args, field, *value);
    append_set(args, shadow_field(present_shadow, field), present_value);
    if (tag_shadow_value) {
        append_set(args, shadow_field(tag_shadow, field), encode_redis_tag(*value));
    }
}

}  // namespace ruvia::detail
