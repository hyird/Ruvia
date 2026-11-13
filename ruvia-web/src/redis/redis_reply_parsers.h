#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/detail/redis/redis_orm.h"
#include "ruvia/web/redis/redis_session.h"

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
