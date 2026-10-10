#include <charconv>
#include <cmath>
#include <limits>
#include <utility>
#include <variant>

#include "ruvia/core/decimal_number.h"
#include "ruvia/web/detail/redis/redis_utils.h"

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
    const auto output = ruvia::parse_decimal_number(value);
    if ((output.index() != 0) || !std::isfinite(std::get<0>(output))) {
        throw redis_error(redis_error::code_type::protocol_error, context_value);
    }
    return std::get<0>(output);
}

std::pmr::vector<redis_key_value> parse_redis_key_value_array(
    const redis_value& value, std::pmr::memory_resource* resource, std::string_view context_value) {
    throw_if_redis_error(value);
    const auto values = redis_value_array(value);
    if (values.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, context_value);
    }
    std::pmr::vector<redis_key_value> result(resource);
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto key = redis_value_string(values[i]);
        const auto field_value = redis_value_string(values[i + 1]);
        result.push_back(redis_types_access::key_value(key, field_value, resource));
    }
    return result;
}

std::pmr::vector<redis_scored_value> parse_redis_scored_array(
    const redis_value& value, std::pmr::memory_resource* resource) {
    throw_if_redis_error(value);
    const auto values = redis_value_array(value);
    if (values.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis scored array reply");
    }
    std::pmr::vector<redis_scored_value> result(resource);
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto member = redis_value_string(values[i]);
        const auto score_text = redis_value_string(values[i + 1]);
        result.push_back(redis_types_access::scored_value(
            member, parse_redis_double(score_text, "invalid redis score"), resource));
    }
    return result;
}

redis_scan_result parse_redis_scan_result(const redis_value& value, std::pmr::memory_resource* resource) {
    throw_if_redis_error(value);
    const auto root = redis_value_array(value);
    if (root.size() != 2) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis scan reply");
    }
    const auto cursor_value = parse_redis_cursor(redis_value_string(root[0]));
    const auto values = redis_value_array(root[1]);
    redis_scan_result result_value = redis_types_access::scan_result(resource);
    redis_types_access::next_cursor(result_value) = cursor_value;
    auto& output_values = redis_types_access::values(result_value);
    output_values.reserve(values.size());
    for (const auto& item : values) {
        const auto text = redis_value_string(item);
        emplace_redis_string(output_values, text);
    }
    return result_value;
}

redis_hash_scan_result parse_redis_hash_scan_result(
    const redis_value& value, std::pmr::memory_resource* resource) {
    throw_if_redis_error(value);
    const auto root = redis_value_array(value);
    if (root.size() != 2) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis hscan reply");
    }
    const auto cursor_value = parse_redis_cursor(redis_value_string(root[0]));
    const auto values = redis_value_array(root[1]);
    if (values.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis hscan entry count");
    }
    redis_hash_scan_result result_value = redis_types_access::hash_scan_result(resource);
    redis_types_access::next_cursor(result_value) = cursor_value;
    auto& output_entries = redis_types_access::entries(result_value);
    output_entries.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto key = redis_value_string(values[i]);
        const auto field_value = redis_value_string(values[i + 1]);
        output_entries.push_back(redis_types_access::key_value(key, field_value, resource));
    }
    return result_value;
}

redis_z_scan_result parse_redis_z_scan_result(
    const redis_value& value, std::pmr::memory_resource* resource) {
    throw_if_redis_error(value);
    const auto root = redis_value_array(value);
    if (root.size() != 2) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis zscan reply");
    }
    const auto cursor_value = parse_redis_cursor(redis_value_string(root[0]));
    const auto values = redis_value_array(root[1]);
    if (values.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis zscan entry count");
    }
    redis_z_scan_result result_value = redis_types_access::z_scan_result(resource);
    redis_types_access::next_cursor(result_value) = cursor_value;
    auto& output_entries = redis_types_access::entries(result_value);
    output_entries.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto member = redis_value_string(values[i]);
        const auto score_text = redis_value_string(values[i + 1]);
        output_entries.push_back(redis_types_access::scored_value(
            member, parse_redis_double(score_text, "invalid redis zscan score"), resource));
    }
    return result_value;
}

std::optional<redis_key_value> parse_redis_blocking_pop_reply(
    const redis_value& value, std::pmr::memory_resource* resource) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }
    const auto items = redis_value_array(value);
    if (items.size() != 2) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis blocking pop reply");
    }
    const auto key = redis_value_string(items[0]);
    const auto item = redis_value_string(items[1]);
    return redis_types_access::key_value(key, item, resource);
}

std::optional<redis_x_read_group_result> parse_redis_x_read_group_reply(
    const redis_value& value, std::pmr::memory_resource* resource) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }

    auto result_value = redis_types_access::xread_group_result(resource);
    const auto stream_values = redis_value_array(value);
    auto& streams = redis_types_access::streams(result_value);
    streams.reserve(stream_values.size());
    for (const auto& stream_value : stream_values) {
        const auto stream_parts = redis_value_array(stream_value);
        if (stream_parts.size() != 2) {
            throw redis_error(
                redis_error::code_type::protocol_error, "unexpected redis xreadgroup stream reply");
        }
        auto stream =
            redis_types_access::stream_read_result(redis_value_string(stream_parts[0]), resource);
        const auto entry_values = redis_value_array(stream_parts[1]);
        auto& entries = redis_types_access::entries(stream);
        entries.reserve(entry_values.size());
        for (const auto& entry_value : entry_values) {
            const auto entry_parts = redis_value_array(entry_value);
            if (entry_parts.size() != 2) {
                throw redis_error(
                    redis_error::code_type::protocol_error, "unexpected redis xreadgroup entry reply");
            }
            auto parsed_entry = redis_types_access::stream_entry(redis_value_string(entry_parts[0]), resource);
            if (entry_parts[1].null()) {
                entries.emplace_back(std::move(parsed_entry));
                continue;
            }
            const auto field_values = redis_value_array(entry_parts[1]);
            if (field_values.size() % 2 != 0) {
                throw redis_error(
                    redis_error::code_type::protocol_error, "unexpected redis xreadgroup field reply");
            }
            auto& fields_value = redis_types_access::fields(parsed_entry);
            fields_value.reserve(field_values.size() / 2);
            for (std::size_t i = 0; i < field_values.size(); i += 2) {
                fields_value.push_back(redis_types_access::key_value(redis_value_string(field_values[i]),
                    redis_value_string(field_values[i + 1]), resource));
            }
            entries.emplace_back(std::move(parsed_entry));
        }
        streams.emplace_back(std::move(stream));
    }
    return result_value;
}

}  // namespace ruvia::detail
