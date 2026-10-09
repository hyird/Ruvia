#pragma once

#include <chrono>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "redis/redis_registry.h"

namespace ruvia::detail {

// Typed reply accessors that enforce the redis_value kind and, on a mismatch,
// throw redis_error(protocol_error) rather than the raw accessors' std::logic_error.
// A reply's type is chosen by the (untrusted) server, so a wrong type is a
// protocol condition the caller catches via redis_error, not a program bug.
[[nodiscard]] std::string_view redis_value_string(const redis_value& value);
[[nodiscard]] std::int64_t redis_value_integer(const redis_value& value);
[[nodiscard]] bool redis_value_integer_bool(const redis_value& value);
[[nodiscard]] std::span<const redis_value> redis_value_array(const redis_value& value);
void throw_if_redis_error(const redis_value& value);
void throw_if_redis_transaction_reply_error(const redis_value& value, std::size_t index);
[[nodiscard]] bool validate_redis_pooled_command(
    std::span<const std::string_view> args, bool allow_blocking);
[[nodiscard]] bool redis_set_returns_previous(redis_set_previous_value_policy policy);
[[nodiscard]] bool redis_x_read_group_uses_no_ack(redis_x_read_group_acknowledgement_policy policy);

[[nodiscard]] std::pmr::vector<std::pmr::string> own_redis_args(
    std::span<const std::string_view> args, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> own_redis_args(
    std::initializer_list<std::string_view> args, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> own_redis_args(std::string_view first,
    std::span<const std::string_view> rest, std::pmr::memory_resource* resource);

task<redis_value> execute_owned_redis_command(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);
task<redis_value> execute_owned_redis_command(redis_pool& pool, std::pmr::vector<std::pmr::string> args,
    operation_options options, std::pmr::memory_resource* resource);
task<std::optional<std::pmr::string>> redis_string_command(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);
task<std::int64_t> redis_integer_command(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);
task<std::pmr::vector<std::pmr::string>> redis_string_array_command(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);
task<std::pmr::vector<bool>> redis_bool_array_command(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);
task<std::pmr::vector<std::optional<std::pmr::string>>> redis_optional_string_array_command(
    redis_command_executor executor, std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource);
task<void> redis_ok_command(redis_command_executor executor, std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource);
task<std::pmr::string> redis_status_command(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::string redis_seconds_string(
    std::chrono::seconds ttl, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string redis_milliseconds_string(
    std::chrono::milliseconds ttl, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string redis_cursor_string(
    std::optional<redis_scan_cursor> cursor_value, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::vector<std::pmr::string> redis_command_with_keys(std::string_view command,
    std::span<const std::string_view> keys, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> redis_mset_args(
    std::span<const std::pair<std::string_view, std::string_view>> items,
    std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> redis_set_args(std::string_view key,
    std::string_view value, const redis_set_options& options, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> redis_hset_fields_args(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields_value,
    std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> redis_command_with_key_fields(std::string_view command,
    std::string_view key, std::span<const std::string_view> fields_value,
    std::pmr::memory_resource* resource);
void append_redis_scan_options(std::pmr::vector<std::pmr::string>& args,
    const redis_scan_options& options, std::pmr::memory_resource* resource);

[[nodiscard]] double parse_redis_double(std::string_view value, std::string_view context_value);
[[nodiscard]] std::pmr::vector<redis_key_value> parse_redis_key_value_array(
    const redis_value& value, std::pmr::memory_resource* resource, std::string_view context_value);
[[nodiscard]] std::pmr::vector<redis_scored_value> parse_redis_scored_array(
    const redis_value& value, std::pmr::memory_resource* resource);
[[nodiscard]] redis_scan_result parse_redis_scan_result(
    const redis_value& value, std::pmr::memory_resource* resource);
[[nodiscard]] redis_hash_scan_result parse_redis_hash_scan_result(
    const redis_value& value, std::pmr::memory_resource* resource);
[[nodiscard]] redis_z_scan_result parse_redis_z_scan_result(
    const redis_value& value, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::vector<std::pmr::string> redis_eval_args(std::string_view command,
    std::string_view script, std::span<const std::string_view> keys,
    std::span<const std::string_view> argv, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> redis_blocking_pop_args(std::string_view command,
    std::span<const std::string_view> keys, redis_block_wait wait,
    std::pmr::memory_resource* resource);
[[nodiscard]] std::optional<redis_key_value> parse_redis_blocking_pop_reply(
    const redis_value& value, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> redis_x_read_group_args(std::string_view group,
    std::string_view consumer, std::span<const redis_stream_read_view> streams,
    const redis_x_read_group_options& options, std::pmr::memory_resource* resource);
[[nodiscard]] std::optional<redis_x_read_group_result> parse_redis_x_read_group_reply(
    const redis_value& value, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
