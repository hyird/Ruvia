#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/detail/redis/redis_owned_command.h"
#include "ruvia/web/redis/redis_handle.h"

#include "redis/redis_protocol.h"

namespace ruvia::detail {

inline constexpr std::size_t redis_int_scratch_size = 20;
inline constexpr std::size_t redis_score_scratch_size = 64;

[[nodiscard]] inline std::string_view format_redis_int(
    std::span<char, redis_int_scratch_size> scratch, std::int64_t value) noexcept {
    const auto end = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value).ptr;
    return {scratch.data(), static_cast<std::size_t>(end - scratch.data())};
}

// Redis accepts "+inf"/"-inf" as sorted-set scores and range bounds, and
// replies them back, so the client round-trips infinities. NaN has no Redis
// spelling and is rejected.
[[nodiscard]] inline std::string_view format_redis_score(
    std::span<char, redis_score_scratch_size> scratch, double value) {
    if (std::isinf(value)) {
        return value > 0 ? "+inf" : "-inf";
    }
    if (!std::isfinite(value)) {
        throw std::invalid_argument("redis sorted set score must not be NaN");
    }
    const auto [ptr, ec] = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("redis sorted set score is invalid");
    }
    return {scratch.data(), static_cast<std::size_t>(ptr - scratch.data())};
}

// Encodes the arguments `enumerate_arguments(emit)` produces, in order, as one
// RESP command. The enumeration runs twice, once to size the request and once
// to append it, so the argument layout is written in exactly one place.
template <typename enumerate_type>
[[nodiscard]] redis_owned_command encode_redis_command(
    std::pmr::memory_resource* resource, const enumerate_type& enumerate_arguments) {
    std::size_t arg_count = 0;
    std::size_t bulk_bytes = 0;
    enumerate_arguments([&](std::string_view arg) {
        ++arg_count;
        add_resp_serialized_size(bulk_bytes, resp_bulk_serialized_size(arg.size()));
    });
    std::size_t size = resp_header_serialized_size(arg_count);
    add_resp_serialized_size(size, bulk_bytes);
    redis_owned_command command{std::pmr::string(pmr_resource_or_default(resource))};
    command.wire_.reserve(size);
    append_resp_header(command.wire_, arg_count);
    enumerate_arguments([&](std::string_view arg) { append_resp_bulk(command.wire_, arg); });
    return command;
}

[[nodiscard]] redis_owned_command make_owned_redis_command(
    std::pmr::memory_resource* resource, std::initializer_list<std::string_view> args);

[[nodiscard]] bool redis_set_returns_previous(redis_set_previous_value_policy policy);

[[nodiscard]] redis_owned_command redis_command_with_keys(std::string_view command,
    std::span<const std::string_view> keys, std::pmr::memory_resource* resource);
[[nodiscard]] redis_owned_command redis_mset_args(
    std::span<const std::pair<std::string_view, std::string_view>> items,
    std::pmr::memory_resource* resource);
[[nodiscard]] redis_owned_command redis_set_args(std::string_view key,
    std::string_view value, const redis_set_options& options, std::pmr::memory_resource* resource);
[[nodiscard]] redis_owned_command redis_hset_fields_args(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields_value,
    std::pmr::memory_resource* resource);
[[nodiscard]] redis_owned_command redis_command_with_key_fields(std::string_view command,
    std::string_view key, std::span<const std::string_view> fields_value,
    std::pmr::memory_resource* resource);
[[nodiscard]] redis_owned_command redis_eval_args(std::string_view command,
    std::string_view script, std::span<const std::string_view> keys,
    std::span<const std::string_view> argv, std::pmr::memory_resource* resource);
[[nodiscard]] redis_owned_command redis_blocking_pop_args(std::string_view command,
    std::span<const std::string_view> keys, redis_block_wait wait,
    std::pmr::memory_resource* resource);
[[nodiscard]] redis_owned_command redis_x_read_group_args(std::string_view group,
    std::string_view consumer, std::span<const redis_stream_read_view> streams,
    const redis_x_read_group_options& options, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
