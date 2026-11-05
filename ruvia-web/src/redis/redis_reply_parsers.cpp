#include "redis/redis_reply_parsers.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/detail/util/decimal_number.h"

#include "redis/redis_command_args.h"
#include "redis/redis_types_access.h"
#include "util/pmr_owned_storage.h"

namespace ruvia::detail {
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

std::pmr::string take_redis_string(redis_value&& value, std::pmr::memory_resource* resource) {
    (void)redis_value_string(value);
    return rebind_owned_storage(redis_types_access::text(value), pmr_resource_or_default(resource));
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

std::optional<std::pmr::string> parse_redis_optional_string(redis_value&& value, std::pmr::memory_resource* resource) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }
    return take_redis_string(std::move(value), resource);
}

std::int64_t parse_redis_integer(redis_value&& value, std::pmr::memory_resource*) {
    return redis_value_integer(value);
}

bool parse_redis_integer_bool(redis_value&& value, std::pmr::memory_resource*) {
    return redis_value_integer_bool(value);
}

void parse_redis_ok(redis_value&& value, std::pmr::memory_resource*) {
    if (!http_ascii_equals_ignore_case(redis_value_string(value), "OK")) {
        throw redis_error(redis_error::code_type::command_error, "unexpected redis status reply");
    }
}

void parse_redis_pong(redis_value&& value, std::pmr::memory_resource*) {
    if (!http_ascii_equals_ignore_case(redis_value_string(value), "PONG")) {
        throw redis_error(redis_error::code_type::command_error, "unexpected redis ping reply");
    }
}

std::pmr::vector<bool> parse_redis_bool_array(redis_value&& value, std::pmr::memory_resource* resource) {
    const auto items = redis_value_array(std::as_const(value));
    std::pmr::vector<bool> output(resource);
    output.reserve(items.size());
    for (const auto& item : items) {
        output.emplace_back(redis_value_integer_bool(item));
    }
    return output;
}

std::pmr::vector<std::optional<std::pmr::string>> parse_redis_optional_string_array(
    redis_value&& value, std::pmr::memory_resource* resource) {
    const auto items = redis_value_array(value);
    std::pmr::vector<std::optional<std::pmr::string>> output(resource);
    output.reserve(items.size());
    for (auto& item : items) {
        output.emplace_back(parse_redis_optional_string(std::move(item), resource));
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

redis_set_result parse_redis_set_reply(
    redis_value&& value, std::pmr::memory_resource* resource, const redis_set_options& options) {
    throw_if_redis_error(value);
    const auto returns_previous = redis_set_returns_previous(options.previous_value_);
    if (value.null()) {
        const bool applied =
            returns_previous &&
            (!options.condition_.has_value() || *options.condition_ == redis_set_condition::if_absent);
        return redis_types_access::set_result(applied);
    }
    if (!returns_previous) {
        if (!http_ascii_equals_ignore_case(redis_value_string(value), "OK")) {
            throw redis_error(redis_error::code_type::command_error, "unexpected redis set reply");
        }
        return redis_types_access::set_result(true);
    }
    const bool applied =
        !options.condition_.has_value() || *options.condition_ != redis_set_condition::if_absent;
    return redis_types_access::set_result(applied, take_redis_string(std::move(value), resource));
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

std::pmr::vector<redis_key_value> parse_redis_key_value_array(
    redis_value&& value, std::pmr::memory_resource* resource, std::string_view context_value) {
    const auto values = redis_value_array(value);
    if (values.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, context_value);
    }
    std::pmr::vector<redis_key_value> result(resource);
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        result.push_back(redis_types_access::key_value(
            take_redis_string(std::move(values[i]), resource),
            take_redis_string(std::move(values[i + 1]), resource)));
    }
    return result;
}

std::pmr::vector<redis_scored_value> parse_redis_scored_array(
    redis_value&& value, std::pmr::memory_resource* resource) {
    const auto values = redis_value_array(value);
    if (values.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis scored array reply");
    }
    std::pmr::vector<redis_scored_value> result(resource);
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto score_text = redis_value_string(values[i + 1]);
        result.push_back(redis_types_access::scored_value(
            take_redis_string(std::move(values[i]), resource),
            parse_redis_double(score_text, "invalid redis score")));
    }
    return result;
}

std::pmr::vector<std::pmr::string> parse_redis_string_array(
    redis_value&& value, std::pmr::memory_resource* resource) {
    const auto items = redis_value_array(value);
    std::pmr::vector<std::pmr::string> output(resource);
    output.reserve(items.size());
    for (auto& item : items) {
        output.emplace_back(take_redis_string(std::move(item), resource));
    }
    return output;
}

namespace {

// SCAN-family replies share one envelope: [cursor, payload]. The payload is
// handed to the matching array parser.
template <typename result_type, typename payload_parser_type>
[[nodiscard]] result_type parse_redis_scan_envelope(redis_value&& value, result_type result_value,
    std::string_view context_value, payload_parser_type parse_payload) {
    const auto root = redis_value_array(value);
    if (root.size() != 2) {
        throw redis_error(redis_error::code_type::protocol_error, context_value);
    }
    redis_types_access::next_cursor(result_value) = parse_redis_cursor(redis_value_string(root[0]));
    parse_payload(result_value, std::move(root[1]));
    return result_value;
}

}  // namespace

redis_scan_result parse_redis_scan_result(redis_value&& value, std::pmr::memory_resource* resource) {
    return parse_redis_scan_envelope(std::move(value), redis_types_access::scan_result(resource), "unexpected redis scan reply",
        [resource](redis_scan_result& result_value, redis_value&& payload) {
            redis_types_access::values(result_value) = parse_redis_string_array(std::move(payload), resource);
        });
}

redis_hash_scan_result parse_redis_hash_scan_result(
    redis_value&& value, std::pmr::memory_resource* resource) {
    return parse_redis_scan_envelope(std::move(value), redis_types_access::hash_scan_result(resource),
        "unexpected redis hscan reply", [resource](redis_hash_scan_result& result_value, redis_value&& payload) {
            redis_types_access::entries(result_value) =
                parse_redis_key_value_array(std::move(payload), resource, "unexpected redis hscan entry count");
        });
}

redis_z_scan_result parse_redis_z_scan_result(
    redis_value&& value, std::pmr::memory_resource* resource) {
    return parse_redis_scan_envelope(std::move(value), redis_types_access::z_scan_result(resource),
        "unexpected redis zscan reply", [resource](redis_z_scan_result& result_value, redis_value&& payload) {
            redis_types_access::entries(result_value) = parse_redis_scored_array(std::move(payload), resource);
        });
}

std::optional<redis_key_value> parse_redis_blocking_pop_reply(
    redis_value&& value, std::pmr::memory_resource* resource) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }
    const auto items = redis_value_array(value);
    if (items.size() != 2) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis blocking pop reply");
    }
    return redis_types_access::key_value(
        take_redis_string(std::move(items[0]), resource),
        take_redis_string(std::move(items[1]), resource));
}

std::optional<redis_x_read_group_result> parse_redis_x_read_group_reply(
    redis_value&& value, std::pmr::memory_resource* resource) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }

    auto result_value = redis_types_access::xread_group_result(resource);
    const auto stream_values = redis_value_array(value);
    auto& streams = redis_types_access::streams(result_value);
    streams.reserve(stream_values.size());
    for (auto& stream_value : stream_values) {
        const auto stream_parts = redis_value_array(stream_value);
        if (stream_parts.size() != 2) {
            throw redis_error(
                redis_error::code_type::protocol_error, "unexpected redis xreadgroup stream reply");
        }
        auto stream =
            redis_types_access::stream_read_result(take_redis_string(std::move(stream_parts[0]), resource));
        const auto entry_values = redis_value_array(stream_parts[1]);
        auto& entries = redis_types_access::entries(stream);
        entries.reserve(entry_values.size());
        for (auto& entry_value : entry_values) {
            const auto entry_parts = redis_value_array(entry_value);
            if (entry_parts.size() != 2) {
                throw redis_error(
                    redis_error::code_type::protocol_error, "unexpected redis xreadgroup entry reply");
            }
            auto parsed_entry = redis_types_access::stream_entry(take_redis_string(std::move(entry_parts[0]), resource));
            if (!entry_parts[1].null()) {
                redis_types_access::fields(parsed_entry) = parse_redis_key_value_array(
                    std::move(entry_parts[1]), resource, "unexpected redis xreadgroup field reply");
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
            return redis_types_access::pubsub_message(
                redis_value_string(items[1]), std::nullopt, redis_value_string(items[2]));
        }
        const bool pattern = kind == "psubscribe" || kind == "punsubscribe";
        const bool subscribed = kind == "subscribe" || kind == "psubscribe";
        if (!subscribed && kind != "unsubscribe" && kind != "punsubscribe") {
            throw redis_error(redis_error::code_type::protocol_error, malformed);
        }
        const auto name = optional_name(items[1]);
        const auto confirmation_kind = pattern
                                           ? (subscribed ? redis_pubsub_confirmation_kind::pattern_subscribed
                                                         : redis_pubsub_confirmation_kind::pattern_unsubscribed)
                                           : (subscribed ? redis_pubsub_confirmation_kind::subscribed
                                                         : redis_pubsub_confirmation_kind::unsubscribed);
        return redis_types_access::pubsub_confirmation(
            confirmation_kind, name, redis_value_integer(items[2]));
    }
    if (kind != "pmessage") {
        throw redis_error(redis_error::code_type::protocol_error, malformed);
    }
    return redis_types_access::pubsub_message(redis_value_string(items[2]),
        redis_value_string(items[1]), redis_value_string(items[3]));
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
    return items.size() == 2 && items[0].kind() == redis_value::kind_type::string &&
           http_ascii_equals_ignore_case(items[0].string(), "pong") &&
           items[1].kind() == redis_value::kind_type::string && items[1].string().empty();
}

}  // namespace ruvia::detail
