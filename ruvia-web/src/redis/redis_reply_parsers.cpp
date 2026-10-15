#include <charconv>
#include <cmath>
#include <limits>
#include <utility>
#include <variant>

#include "ruvia/web/detail/util/decimal_number.h"

#include "redis/redis_handle_helpers.h"
#include "redis/redis_types_access.h"

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
    if ((output.index() != 0) || !std::isfinite(std::get<0>(output))) {
        throw redis_error(redis_error::code_type::protocol_error, context_value);
    }
    return std::get<0>(output);
}

std::pmr::vector<redis_key_value> parse_redis_key_value_array(
    redis_value&& value, std::pmr::memory_resource* resource, std::string_view context_value) {
    throw_if_redis_error(value);
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
    throw_if_redis_error(value);
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
    throw_if_redis_error(value);
    const auto items = redis_value_array(value);
    std::pmr::vector<std::pmr::string> output(resource);
    output.reserve(items.size());
    for (auto& item : items) {
        throw_if_redis_error(item);
        if (!item.null()) {
            output.emplace_back(take_redis_string(std::move(item), resource));
        }
    }
    return output;
}

namespace {

// SCAN-family replies share one envelope: [cursor, payload]. The payload is
// handed to the matching array parser.
template <typename result_type, typename payload_parser_type>
[[nodiscard]] result_type parse_redis_scan_envelope(redis_value&& value, result_type result_value,
    std::string_view context_value, payload_parser_type parse_payload) {
    throw_if_redis_error(value);
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

}  // namespace ruvia::detail
