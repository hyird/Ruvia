#include "redis_codec.h"

#include <hiredis/hiredis.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "http_header.h"
#include "json_codec.h"
#include "redis.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] std::size_t resp_decimal_size(std::size_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("redis RESP length is too large");
    }
    return http_unsigned_decimal_size(static_cast<std::uint64_t>(value));
}

template <typename args_type>
[[nodiscard]] std::size_t resp_serialized_size(const args_type& args) {
    if (args.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    std::size_t size = resp_header_serialized_size(args.size());
    for (const auto& arg : args) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(static_cast<std::size_t>(arg.size())));
    }
    return size;
}

// Writes the RESP multi-bulk encoding of `args` to `output`. Callers reserve
// the required capacity up front (per command in append_resp_command(), or once
// for the whole batch in redis_pool::execute_commands()).
template <typename args_type>
void serialize_resp_command_impl(std::string& output, const args_type& args) {
    append_resp_header(output, args.size());
    for (const auto& arg : args) {
        append_resp_bulk(output, std::string_view(arg.data(), static_cast<std::size_t>(arg.size())));
    }
}

template <typename args_type>
void append_resp_command_impl(std::string& output, const args_type& args) {
    const auto serialized_size = resp_serialized_size(args);
    if (serialized_size > std::numeric_limits<std::size_t>::max() - output.size()) {
        throw std::length_error("redis RESP output is too large");
    }
    output.reserve(output.size() + serialized_size);
    serialize_resp_command_impl(output, args);
}

[[nodiscard]] redis_reply_builder& builder_of(const redisReadTask* task_value) noexcept {
    return *static_cast<redis_reply_builder*>(task_value->privdata);
}

}  // namespace

void add_resp_serialized_size(std::size_t& current, std::size_t amount) {
    if (amount > std::numeric_limits<std::size_t>::max() - current) {
        throw std::length_error("redis RESP command is too large");
    }
    current += amount;
}

std::size_t resp_header_serialized_size(std::size_t arg_count) {
    if (arg_count == 0) {
        throw std::invalid_argument("redis command must not be empty");
    }
    std::size_t size = 0;
    add_resp_serialized_size(size, 1);
    add_resp_serialized_size(size, resp_decimal_size(arg_count));
    add_resp_serialized_size(size, 2);
    return size;
}

std::size_t resp_bulk_serialized_size(std::size_t arg_bytes) {
    std::size_t size = 0;
    add_resp_serialized_size(size, 1);
    add_resp_serialized_size(size, resp_decimal_size(arg_bytes));
    add_resp_serialized_size(size, 2);
    add_resp_serialized_size(size, arg_bytes);
    add_resp_serialized_size(size, 2);
    return size;
}

void append_resp_header(std::string& output, std::size_t arg_count) {
    output.push_back('*');
    http_append_unsigned(output, static_cast<std::uint64_t>(arg_count));
    output.append("\r\n", 2);
}

void append_resp_bulk(std::string& output, std::string_view arg) {
    output.push_back('$');
    http_append_unsigned(output, static_cast<std::uint64_t>(arg.size()));
    output.append("\r\n", 2);
    if (!arg.empty()) {
        output.append(arg.data(), arg.size());
    }
    output.append("\r\n", 2);
}

void serialize_resp_command(std::string& output, std::span<const std::string_view> args) {
    serialize_resp_command_impl(output, args);
}

void serialize_resp_command(std::string& output, std::span<const std::string> args) {
    serialize_resp_command_impl(output, args);
}

void serialize_resp_command(std::string& output, std::string_view first, std::span<const std::string_view> rest) {
    if (rest.size() == std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("redis RESP command is too large");
    }
    append_resp_header(output, rest.size() + 1U);
    append_resp_bulk(output, first);
    for (const auto arg : rest) {
        append_resp_bulk(output, arg);
    }
}

void append_resp_command(std::string& output, std::span<const std::string_view> args) {
    append_resp_command_impl(output, args);
}

void append_resp_command(std::string& output, std::span<const std::string> args) {
    append_resp_command_impl(output, args);
}

void append_resp_command(std::string& output, std::string_view first, std::span<const std::string_view> rest) {
    const auto serialized_size = resp_command_serialized_size(first, rest);
    if (serialized_size > std::numeric_limits<std::size_t>::max() - output.size()) {
        throw std::length_error("redis RESP output is too large");
    }
    output.reserve(output.size() + serialized_size);
    serialize_resp_command(output, first, rest);
}

std::size_t resp_command_serialized_size(std::span<const std::string_view> args) {
    return resp_serialized_size(args);
}

std::size_t resp_command_serialized_size(std::span<const std::string> args) {
    return resp_serialized_size(args);
}

std::size_t resp_command_serialized_size(std::string_view first, std::span<const std::string_view> rest) {
    if (rest.size() == std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("redis RESP command is too large");
    }
    std::size_t size = resp_header_serialized_size(rest.size() + 1U);
    add_resp_serialized_size(size, resp_bulk_serialized_size(first.size()));
    for (const auto arg : rest) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(arg.size()));
    }
    return size;
}

const char* hiredis_reader_error(const redisReader& reader_value) noexcept {
    return reader_value.errstr[0] == '\0' ? "redis protocol error" : reader_value.errstr;
}

redisReplyObjectFunctions redis_reply_builder::functions_{
    &redis_reply_builder::create_string,
    &redis_reply_builder::create_array,
    &redis_reply_builder::create_integer,
    &redis_reply_builder::create_double,
    &redis_reply_builder::create_nil,
    &redis_reply_builder::create_bool,
    &redis_reply_builder::free_object,
};

redis_reply_builder::redis_reply_builder(std::optional<std::size_t> max_reply_bytes, std::size_t max_depth) noexcept :
    max_reply_bytes_(max_reply_bytes),
    maximum_elements_(max_reply_bytes.has_value() ? *max_reply_bytes / 3 : std::numeric_limits<std::size_t>::max()),
    remaining_elements_(maximum_elements_),
    max_depth_(max_depth) {}

redisReader* redis_reply_builder::create_reader() {
    auto* reader_value = redisReaderCreateWithFunctions(&functions_);
    if (reader_value == nullptr) {
        throw redis_error(redis_error::code_type::protocol_error, "failed to create redis reader");
    }
    reader_value->privdata = this;
    if (max_reply_bytes_.has_value()) {
        // Rejects an oversized array header before any element arrives.
        // Every element requires at least three RESP bytes (e.g. "+\r\n").
        const auto elements = std::max<std::size_t>(1, *max_reply_bytes_ / 3);
        reader_value->maxelements = static_cast<long long>(std::min(elements, static_cast<std::size_t>(REDIS_READER_MAX_ARRAY_ELEMENTS)));
    }
    reader_ = reader_value;
    return reader_value;
}

void redis_reply_builder::begin() noexcept {
    if (root_) {
        std::terminate();
    }
    remaining_elements_ = maximum_elements_;
    rejection_ = rejection_type::none;
    failure_ = nullptr;
}

void redis_reply_builder::throw_if_rejected() {
    const auto rejection = rejection_;
    auto failure = std::move(failure_);
    if (rejection != rejection_type::none) {
        clear();
    }
    switch (rejection) {
        case rejection_type::none:
            return;
        case rejection_type::elements:
            throw redis_error(redis_error::code_type::protocol_error, "redis reply exceeds configured element limit");
        case rejection_type::depth:
            throw redis_error(redis_error::code_type::protocol_error, "redis array nesting is too deep");
        case rejection_type::failed:
            std::rethrow_exception(failure);
    }
}

void redis_reply_builder::clear() noexcept {
    root_.reset();
    remaining_elements_ = maximum_elements_;
    rejection_ = rejection_type::none;
    failure_ = nullptr;
}

redis_value redis_reply_builder::take(void* reply) noexcept {
    auto* root = static_cast<root_slot*>(reply);
    auto* owner = root->owner_;
    redis_value value(std::move(root->value_));
    owner->root_.reset();
    return value;
}

// hiredis creates the elements of an array in index order, so each child is
// appended to its parent. Only the innermost open array grows and hiredis
// keeps no pointer to its earlier, completed elements, so a reallocation never
// invalidates a node the reader still refers to. The root stays in this
// connection's builder and hiredis only passes that root to free_object().
template <typename factory_type>
void* redis_reply_builder::place(const redisReadTask* task_value, factory_type&& factory) noexcept {
    try {
        if (task_value->parent != nullptr) {
            auto* parent = task_value->parent->parent == nullptr ? &static_cast<root_slot*>(task_value->parent->obj)->value_ : static_cast<redis_value*>(task_value->parent->obj);
            auto& siblings = redis_types_access::elements(*parent);
            siblings.push_back(factory());
            return &siblings.back();
        }
        root_.emplace(*this, factory());
        return &*root_;
    } catch (...) {
        rejection_ = rejection_type::failed;
        failure_ = std::current_exception();
        return nullptr;
    }
}

void* redis_reply_builder::create_string(const redisReadTask* task_value, char* data, std::size_t size) noexcept {
    auto& builder = builder_of(task_value);
    std::string_view text(data, size);
    if (task_value->type == REDIS_REPLY_VERB) {
        // The reader has verified the "fmt:" prefix; the value excludes it.
        text.remove_prefix(4);
    }
    return builder.place(task_value, [&] {
        return task_value->type == REDIS_REPLY_ERROR ? redis_types_access::error_value(text) : redis_types_access::string_value(text);
    });
}

void* redis_reply_builder::create_array(const redisReadTask* task_value, std::size_t elements) noexcept {
    auto& builder = builder_of(task_value);
    std::size_t depth = 0;
    for (auto* parent_value = task_value; parent_value != nullptr; parent_value = parent_value->parent) {
        if (++depth > builder.max_depth_) {
            builder.rejection_ = rejection_type::depth;
            return nullptr;
        }
    }
    if (elements > builder.remaining_elements_) {
        builder.rejection_ = rejection_type::elements;
        return nullptr;
    }
    // Reserve only what the buffered bytes can already describe; a header
    // announcing more elements grows the array as they actually arrive.
    const auto buffered = builder.reader_->len - builder.reader_->pos;
    const auto capacity = std::min(elements, buffered / 3);
    auto* node = builder.place(task_value, [&] {
        return redis_types_access::array_value(capacity);
    });
    if (node != nullptr) {
        builder.remaining_elements_ -= elements;
    }
    return node;
}

void* redis_reply_builder::create_integer(const redisReadTask* task_value, long long value) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] {
        return redis_types_access::integer_value(static_cast<std::int64_t>(value));
    });
}

void* redis_reply_builder::create_double(const redisReadTask* task_value, double, char* data, std::size_t size) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] {
        return redis_types_access::string_value(std::string_view(data, size));
    });
}

void* redis_reply_builder::create_nil(const redisReadTask* task_value) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] {
        return redis_types_access::null_value();
    });
}

void* redis_reply_builder::create_bool(const redisReadTask* task_value, int value) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] {
        return redis_types_access::integer_value(value == 0 ? 0 : 1);
    });
}

void redis_reply_builder::free_object(void* reply) noexcept {
    auto* root = static_cast<root_slot*>(reply);
    root->owner_->root_.reset();
}

namespace {

[[nodiscard]] std::optional<redis_scan_cursor> parse_redis_cursor(std::string_view value) {
    std::uint64_t cursor_value = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), cursor_value);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid redis scan cursor");
    }
    if (cursor_value == 0) {
        return std::nullopt;
    }
    return redis_types_access::scan_cursor(cursor_value);
}

}  // namespace

std::string_view redis_value_string(const redis_value& value) {
    throw_if_redis_error(value);
    if (value.kind() != redis_value::kind_type::string) {
        throw redis_error(redis_error::code_type::protocol_error, "redis reply is not a string");
    }
    return value.string();
}

std::string take_redis_string(redis_value&& value) {
    (void)redis_value_string(value);
    return std::move(redis_types_access::text(value));
}

std::int64_t redis_value_integer(const redis_value& value) {
    throw_if_redis_error(value);
    if (value.kind() != redis_value::kind_type::integer) {
        throw redis_error(redis_error::code_type::protocol_error, "redis reply is not an integer");
    }
    return value.integer();
}

bool redis_value_integer_bool(const redis_value& value) {
    const auto integer = redis_value_integer(value);
    if (integer == 0) {
        return false;
    }
    if (integer == 1) {
        return true;
    }
    throw redis_error(redis_error::code_type::protocol_error, "redis boolean reply is not 0 or 1");
}

std::span<const redis_value> redis_value_array(const redis_value& value) {
    throw_if_redis_error(value);
    if (value.kind() != redis_value::kind_type::array) {
        throw redis_error(redis_error::code_type::protocol_error, "redis reply is not an array");
    }
    return value.array();
}

std::span<redis_value> redis_value_array(redis_value& value) {
    (void)redis_value_array(std::as_const(value));
    return redis_types_access::elements(value);
}

void throw_if_redis_error(const redis_value& value) {
    if (value.kind() == redis_value::kind_type::error) {
        throw redis_error(redis_error::code_type::command_error, value.error());
    }
}

void throw_if_redis_transaction_reply_error(const redis_value& value, std::size_t index) {
    if (value.kind() != redis_value::kind_type::error) {
        return;
    }
    std::array<char, http_max_unsigned_decimal_size> index_digits;
    const auto index_text = http_format_unsigned(index_digits, index);
    std::string message;
    message.reserve(26 + index_text.size() + value.error().size());
    message.append("redis transaction reply ");
    message.append(index_text);
    message.append(": ");
    message.append(value.error());
    throw redis_error(redis_error::code_type::command_error, message);
}

std::optional<std::string> parse_redis_optional_string(redis_value&& value) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }
    return take_redis_string(std::move(value));
}

std::int64_t parse_redis_integer(redis_value&& value) {
    return redis_value_integer(value);
}

bool parse_redis_integer_bool(redis_value&& value) {
    return redis_value_integer_bool(value);
}

void parse_redis_ok(redis_value&& value) {
    if (!http_ascii_equals_ignore_case(redis_value_string(value), "OK")) {
        throw redis_error(redis_error::code_type::command_error, "unexpected redis status reply");
    }
}

void parse_redis_pong(redis_value&& value) {
    if (!http_ascii_equals_ignore_case(redis_value_string(value), "PONG")) {
        throw redis_error(redis_error::code_type::command_error, "unexpected redis ping reply");
    }
}

std::vector<bool> parse_redis_bool_array(redis_value&& value) {
    const auto items = redis_value_array(std::as_const(value));
    std::vector<bool> output;
    output.reserve(items.size());
    for (const auto& item : items) {
        output.emplace_back(redis_value_integer_bool(item));
    }
    return output;
}

std::vector<std::optional<std::string>> parse_redis_optional_string_array(redis_value&& value) {
    const auto items = redis_value_array(value);
    std::vector<std::optional<std::string>> output;
    output.reserve(items.size());
    for (auto& item : items) {
        output.emplace_back(parse_redis_optional_string(std::move(item)));
    }
    return output;
}

std::optional<double> parse_redis_optional_double(redis_value&& value, std::string_view context_value) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }
    return parse_redis_double(redis_value_string(value), context_value);
}

redis_set_result parse_redis_set_reply(redis_value&& value, const redis_set_options& options) {
    throw_if_redis_error(value);
    const auto returns_previous = redis_set_returns_previous(options.previous_value_);
    if (value.null()) {
        const bool applied = returns_previous && (!options.condition_.has_value() || *options.condition_ == redis_set_condition::if_absent);
        return redis_types_access::set_result(applied);
    }
    if (!returns_previous) {
        if (!http_ascii_equals_ignore_case(redis_value_string(value), "OK")) {
            throw redis_error(redis_error::code_type::command_error, "unexpected redis set reply");
        }
        return redis_types_access::set_result(true);
    }
    const bool applied = !options.condition_.has_value() || *options.condition_ != redis_set_condition::if_absent;
    return redis_types_access::set_result(applied, take_redis_string(std::move(value)));
}

redis_ttl parse_redis_ttl(redis_value&& value, bool seconds_precision) {
    const auto integer = redis_value_integer(value);
    if (integer == -2) {
        return redis_types_access::ttl(redis_ttl_state::missing);
    }
    if (integer == -1) {
        return redis_types_access::ttl(redis_ttl_state::persistent);
    }
    if (integer < 0) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid redis TTL reply");
    }
    using milliseconds_type = std::chrono::milliseconds;
    auto milliseconds = integer;
    if (seconds_precision) {
        constexpr auto scale = milliseconds_type(std::chrono::seconds(1)).count();
        if (integer > milliseconds_type::max().count() / scale) {
            throw redis_error(redis_error::code_type::protocol_error, "redis TTL reply exceeds milliseconds range");
        }
        milliseconds *= scale;
    }
    return redis_types_access::ttl(redis_ttl_state::expiring, milliseconds_type(milliseconds));
}

// Redis replies non-finite doubles textually: scores added as +inf/-inf come
// back as "inf"/"-inf", and RESP3-style doubles may also be "nan". Every other
// spelling must be a plain finite decimal.
double parse_redis_double(std::string_view value, std::string_view context_value) {
    if (value == "inf" || value == "+inf") {
        return std::numeric_limits<double>::infinity();
    }
    if (value == "-inf") {
        return -std::numeric_limits<double>::infinity();
    }
    if (value == "nan") {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto output = detail::parse_decimal_number(value);
    if (!output || !std::isfinite(*output)) {
        throw redis_error(redis_error::code_type::protocol_error, context_value);
    }
    return *output;
}

std::vector<redis_key_value> parse_redis_key_value_array(redis_value&& value, std::string_view context_value) {
    const auto values = redis_value_array(value);
    if (values.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, context_value);
    }
    std::vector<redis_key_value> result;
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        result.push_back(redis_types_access::key_value(take_redis_string(std::move(values[i])), take_redis_string(std::move(values[i + 1]))));
    }
    return result;
}

std::vector<redis_scored_value> parse_redis_scored_array(redis_value&& value) {
    const auto values = redis_value_array(value);
    if (values.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis scored array reply");
    }
    std::vector<redis_scored_value> result;
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto score_text = redis_value_string(values[i + 1]);
        result.push_back(redis_types_access::scored_value(take_redis_string(std::move(values[i])), parse_redis_double(score_text, "invalid redis score")));
    }
    return result;
}

std::vector<std::string> parse_redis_string_array(redis_value&& value) {
    const auto items = redis_value_array(value);
    std::vector<std::string> output;
    output.reserve(items.size());
    for (auto& item : items) {
        output.emplace_back(take_redis_string(std::move(item)));
    }
    return output;
}

namespace {

// SCAN-family replies share one envelope: [cursor, payload]. The payload is
// handed to the matching array parser.
template <typename result_type, typename payload_parser_type>
[[nodiscard]] result_type parse_redis_scan_envelope(redis_value&& value, result_type result_value, std::string_view context_value, payload_parser_type parse_payload) {
    const auto root = redis_value_array(value);
    if (root.size() != 2) {
        throw redis_error(redis_error::code_type::protocol_error, context_value);
    }
    redis_types_access::next_cursor(result_value) = parse_redis_cursor(redis_value_string(root[0]));
    parse_payload(result_value, std::move(root[1]));
    return result_value;
}

}  // namespace

redis_scan_result parse_redis_scan_result(redis_value&& value) {
    return parse_redis_scan_envelope(std::move(value), redis_types_access::scan_result(), "unexpected redis scan reply", [](redis_scan_result& result_value, redis_value&& payload) {
        redis_types_access::values(result_value) = parse_redis_string_array(std::move(payload));
    });
}

redis_hash_scan_result parse_redis_hash_scan_result(redis_value&& value) {
    return parse_redis_scan_envelope(std::move(value), redis_types_access::hash_scan_result(), "unexpected redis hscan reply", [](redis_hash_scan_result& result_value, redis_value&& payload) {
        redis_types_access::entries(result_value) = parse_redis_key_value_array(std::move(payload), "unexpected redis hscan entry count");
    });
}

redis_z_scan_result parse_redis_z_scan_result(redis_value&& value) {
    return parse_redis_scan_envelope(std::move(value), redis_types_access::z_scan_result(), "unexpected redis zscan reply", [](redis_z_scan_result& result_value, redis_value&& payload) {
        redis_types_access::entries(result_value) = parse_redis_scored_array(std::move(payload));
    });
}

std::optional<redis_key_value> parse_redis_blocking_pop_reply(redis_value&& value) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }
    const auto items = redis_value_array(value);
    if (items.size() != 2) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis blocking pop reply");
    }
    return redis_types_access::key_value(take_redis_string(std::move(items[0])), take_redis_string(std::move(items[1])));
}

std::optional<redis_x_read_group_result> parse_redis_x_read_group_reply(redis_value&& value) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }

    auto result_value = redis_types_access::xread_group_result();
    const auto stream_values = redis_value_array(value);
    auto& streams = redis_types_access::streams(result_value);
    streams.reserve(stream_values.size());
    for (auto& stream_value : stream_values) {
        const auto stream_parts = redis_value_array(stream_value);
        if (stream_parts.size() != 2) {
            throw redis_error(redis_error::code_type::protocol_error, "unexpected redis xreadgroup stream reply");
        }
        auto stream = redis_types_access::stream_read_result(take_redis_string(std::move(stream_parts[0])));
        const auto entry_values = redis_value_array(stream_parts[1]);
        auto& entries = redis_types_access::entries(stream);
        entries.reserve(entry_values.size());
        for (auto& entry_value : entry_values) {
            const auto entry_parts = redis_value_array(entry_value);
            if (entry_parts.size() != 2) {
                throw redis_error(redis_error::code_type::protocol_error, "unexpected redis xreadgroup entry reply");
            }
            auto parsed_entry = redis_types_access::stream_entry(take_redis_string(std::move(entry_parts[0])));
            if (!entry_parts[1].null()) {
                redis_types_access::fields(parsed_entry) = parse_redis_key_value_array(std::move(entry_parts[1]), "unexpected redis xreadgroup field reply");
            }
            entries.emplace_back(std::move(parsed_entry));
        }
        streams.emplace_back(std::move(stream));
    }
    return result_value;
}

// RESP2 subscribed-mode pushes are arrays led by their kind: confirmations
// carry the name (nil when UNSUBSCRIBE finds nothing to leave) and the
// remaining subscription count; messages carry the channel and payload, with
// the matched pattern first for a pattern subscription.
redis_pubsub_event parse_redis_pubsub_event(const redis_value& value) {
    constexpr std::string_view malformed = "unexpected redis pub/sub push";
    const auto items = redis_value_array(value);
    if (items.size() < 3 || items.size() > 4) {
        throw redis_error(redis_error::code_type::protocol_error, malformed);
    }
    const auto kind = redis_value_string(items[0]);
    const auto optional_name = [&](const redis_value& item) -> std::optional<std::string_view> {
        return item.null() ? std::nullopt : std::optional(redis_value_string(item));
    };
    if (items.size() == 3) {
        if (kind == "message") {
            return redis_types_access::pubsub_message(redis_value_string(items[1]), std::nullopt, redis_value_string(items[2]));
        }
        const bool pattern = kind == "psubscribe" || kind == "punsubscribe";
        const bool subscribed = kind == "subscribe" || kind == "psubscribe";
        if (!subscribed && kind != "unsubscribe" && kind != "punsubscribe") {
            throw redis_error(redis_error::code_type::protocol_error, malformed);
        }
        const auto name = optional_name(items[1]);
        const auto confirmation_kind = pattern ? (subscribed ? redis_pubsub_confirmation_kind::pattern_subscribed : redis_pubsub_confirmation_kind::pattern_unsubscribed) : (subscribed ? redis_pubsub_confirmation_kind::subscribed : redis_pubsub_confirmation_kind::unsubscribed);
        return redis_types_access::pubsub_confirmation(confirmation_kind, name, redis_value_integer(items[2]));
    }
    if (kind != "pmessage") {
        throw redis_error(redis_error::code_type::protocol_error, malformed);
    }
    return redis_types_access::pubsub_message(redis_value_string(items[2]), redis_value_string(items[1]), redis_value_string(items[3]));
}

// RESP2 answers PING on a subscribed connection with the push-shaped
// ["pong", <argument>]; once the last subscription is gone the connection is
// back in normal mode and answers +PONG, which is also the RESP3 reply. An
// argument-less PING echoes an empty argument.
bool is_redis_subscription_pong(const redis_value& value) {
    if (value.kind() == redis_value::kind_type::string) {
        return http_ascii_equals_ignore_case(value.string(), "PONG");
    }
    if (value.kind() != redis_value::kind_type::array) {
        return false;
    }
    const auto items = value.array();
    return items.size() == 2 && items[0].kind() == redis_value::kind_type::string && http_ascii_equals_ignore_case(items[0].string(), "pong") && items[1].kind() == redis_value::kind_type::string && items[1].string().empty();
}

}  // namespace ruvia::detail

namespace ruvia {

redis_error::redis_error(code_type code, std::string_view message) :
    std::runtime_error(std::string(message)),
    code_(code) {}

redis_error::code_type redis_error::code() const noexcept {
    return code_;
}

redis_value::redis_value() noexcept = default;

redis_value& redis_value::operator=(const redis_value& other) {
    if (this != &other) {
        redis_value owned(other);
        string_.swap(owned.string_);
        array_.swap(owned.array_);
        kind_ = owned.kind_;
        integer_ = owned.integer_;
    }
    return *this;
}

redis_value& redis_value::operator=(redis_value&& other) noexcept {
    if (this != &other) {
        redis_value owned(std::move(other));
        string_.swap(owned.string_);
        array_.swap(owned.array_);
        kind_ = owned.kind_;
        integer_ = owned.integer_;
    }
    return *this;
}

redis_value::kind_type redis_value::kind() const noexcept {
    return kind_;
}

bool redis_value::null() const noexcept {
    return kind_ == kind_type::null;
}

std::string_view redis_value::string() const& {
    if (kind_ != kind_type::string) {
        throw std::logic_error("redis value is not a string");
    }
    return string_;
}

std::string_view redis_value::error() const& {
    if (kind_ != kind_type::error) {
        throw std::logic_error("redis value is not an error");
    }
    return string_;
}

std::int64_t redis_value::integer() const {
    if (kind_ != kind_type::integer) {
        throw std::logic_error("redis value is not an integer");
    }
    return integer_;
}

std::span<const redis_value> redis_value::array() const& {
    if (kind_ != kind_type::array) {
        throw std::logic_error("redis value is not an array");
    }
    return array_;
}

redis_value redis_value::null_value() noexcept {
    redis_value value;
    value.kind_ = kind_type::null;
    return value;
}

redis_value redis_value::string_value(std::string_view input) {
    redis_value value;
    value.kind_ = kind_type::string;
    value.string_.assign(input.data(), input.size());
    return value;
}

redis_value redis_value::error_value(std::string_view input) {
    redis_value value;
    value.kind_ = kind_type::error;
    value.string_.assign(input.data(), input.size());
    return value;
}

redis_value redis_value::integer_value(std::int64_t input) noexcept {
    redis_value value;
    value.kind_ = kind_type::integer;
    value.integer_ = input;
    return value;
}

redis_value redis_value::array_value(std::size_t capacity) {
    redis_value value;
    value.kind_ = kind_type::array;
    value.array_.reserve(capacity);
    return value;
}

}  // namespace ruvia
