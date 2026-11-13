#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/redis/redis_repository.h"
#include "ruvia/web/redis/redis_session.h"
#include "ruvia/web/redis/redis_types.h"

namespace ruvia::detail {

struct redis_types_access final {
    [[nodiscard]] static redis_set_result set_result(bool applied,
        std::optional<std::string> previous = std::nullopt) {
        return redis_set_result(applied, std::move(previous));
    }

    [[nodiscard]] static constexpr redis_scan_cursor scan_cursor(std::uint64_t value) noexcept {
        return redis_scan_cursor(value);
    }

    [[nodiscard]] static constexpr std::uint64_t cursor_value(redis_scan_cursor cursor_value) noexcept {
        return cursor_value.value_;
    }

    [[nodiscard]] static redis_key_value key_value(
        std::string&& key, std::string&& value) noexcept {
        return redis_key_value(std::move(key), std::move(value));
    }

    [[nodiscard]] static redis_scored_value scored_value(
        std::string&& value, double score) noexcept {
        return redis_scored_value(std::move(value), score);
    }

    [[nodiscard]] static redis_scan_result scan_result() {
        return redis_scan_result{};
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(
        redis_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::vector<std::string>& values(
        redis_scan_result& result_value) noexcept {
        return result_value.values_;
    }

    [[nodiscard]] static redis_hash_scan_result hash_scan_result() {
        return redis_hash_scan_result{};
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(
        redis_hash_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::vector<redis_key_value>& entries(
        redis_hash_scan_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_z_scan_result z_scan_result() {
        return redis_z_scan_result{};
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(
        redis_z_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::vector<redis_scored_value>& entries(
        redis_z_scan_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_stream_entry stream_entry(
        std::string&& id) noexcept {
        return redis_stream_entry(std::move(id));
    }

    [[nodiscard]] static std::vector<redis_key_value>& fields(redis_stream_entry& entry_value) noexcept {
        return entry_value.fields_;
    }

    [[nodiscard]] static redis_stream_read_result stream_read_result(
        std::string&& stream) noexcept {
        return redis_stream_read_result(std::move(stream));
    }

    [[nodiscard]] static std::vector<redis_stream_entry>& entries(
        redis_stream_read_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_x_read_group_result xread_group_result() {
        return redis_x_read_group_result{};
    }

    [[nodiscard]] static std::vector<redis_stream_read_result>& streams(
        redis_x_read_group_result& result_value) noexcept {
        return result_value.streams_;
    }

    [[nodiscard]] static constexpr redis_ttl ttl(redis_ttl_state state_value,
        std::optional<std::chrono::milliseconds> remaining = std::nullopt) noexcept {
        return redis_ttl(state_value, remaining);
    }

    [[nodiscard]] static redis_pubsub_event pubsub_confirmation(redis_pubsub_confirmation_kind kind,
        std::optional<std::string_view> name, std::int64_t subscription_count) noexcept {
        return redis_pubsub_event(redis_pubsub_confirmation(kind, name, subscription_count));
    }
    [[nodiscard]] static redis_pubsub_event pubsub_message(std::string_view channel,
        std::optional<std::string_view> pattern, std::string_view payload) noexcept {
        return redis_pubsub_event(redis_pubsub_message(channel, pattern, payload));
    }

    [[nodiscard]] static redis_value null_value() {
        return redis_value::null_value();
    }

    [[nodiscard]] static redis_value string_value(
        std::string_view value) {
        return redis_value::string_value(value);
    }

    [[nodiscard]] static redis_value error_value(
        std::string_view value) {
        return redis_value::error_value(value);
    }

    [[nodiscard]] static redis_value integer_value(
        std::int64_t value) {
        return redis_value::integer_value(value);
    }

    [[nodiscard]] static redis_value array_value(std::size_t capacity) {
        return redis_value::array_value(capacity);
    }

    [[nodiscard]] static std::vector<redis_value>& elements(redis_value& value) noexcept {
        return value.array_;
    }

    [[nodiscard]] static std::string& text(redis_value& value) noexcept {
        return value.string_;
    }

    // The resource every allocation of `value` comes from.
};

}  // namespace ruvia::detail

#pragma once

namespace ruvia::detail {

// Consumes reply storage when its resource matches; otherwise owns a copy in
// the destination resource. Never returns a borrow into the discarded reply.
[[nodiscard]] std::string take_redis_string(redis_value&& value);
[[nodiscard]] bool redis_value_integer_bool(const redis_value& value);
[[nodiscard]] std::span<redis_value> redis_value_array(redis_value& value);
void throw_if_redis_transaction_reply_error(const redis_value& value, std::size_t index);

// Reply decoders. Those taking only (redis_value&&, memory_resource*) are
// mappers for map_redis_command; results own storage in `resource`.
[[nodiscard]] std::optional<std::string> parse_redis_optional_string(
    redis_value&& value);
[[nodiscard]] std::int64_t parse_redis_integer(redis_value&& value);
[[nodiscard]] bool parse_redis_integer_bool(redis_value&& value);
// Accept only the OK / PONG status reply, compared case-insensitively.
void parse_redis_ok(redis_value&& value);
void parse_redis_pong(redis_value&& value);
[[nodiscard]] std::vector<bool> parse_redis_bool_array(redis_value&& value);
// Every element must be a string; a nil element is a protocol error.
[[nodiscard]] std::vector<std::string> parse_redis_string_array(
    redis_value&& value);
[[nodiscard]] std::vector<std::optional<std::string>> parse_redis_optional_string_array(
    redis_value&& value);
[[nodiscard]] std::vector<redis_scored_value> parse_redis_scored_array(
    redis_value&& value);
[[nodiscard]] redis_scan_result parse_redis_scan_result(redis_value&& value);
[[nodiscard]] redis_hash_scan_result parse_redis_hash_scan_result(
    redis_value&& value);
[[nodiscard]] redis_z_scan_result parse_redis_z_scan_result(redis_value&& value);
[[nodiscard]] std::optional<redis_key_value> parse_redis_blocking_pop_reply(
    redis_value&& value);
[[nodiscard]] std::optional<redis_x_read_group_result> parse_redis_x_read_group_reply(
    redis_value&& value);

[[nodiscard]] double parse_redis_double(std::string_view value, std::string_view context_value);
[[nodiscard]] std::optional<double> parse_redis_optional_double(redis_value&& value, std::string_view context_value);
[[nodiscard]] std::vector<redis_key_value> parse_redis_key_value_array(
    redis_value&& value, std::string_view context_value);
[[nodiscard]] redis_set_result parse_redis_set_reply(
    redis_value&& value, const redis_set_options& options);
// TTL/PTTL: -2 is a missing key, -1 a key without expiry.
[[nodiscard]] redis_ttl parse_redis_ttl(redis_value&& value, bool seconds_precision);
// A pushed reply of a subscribed connection. Views borrow `value`.
[[nodiscard]] redis_pubsub_event parse_redis_pubsub_event(const redis_value& value);
// The reply to an argument-less PING on a subscription connection.
[[nodiscard]] bool is_redis_subscription_pong(const redis_value& value);

}  // namespace ruvia::detail
