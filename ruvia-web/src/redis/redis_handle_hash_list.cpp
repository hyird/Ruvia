#include <array>
#include <string_view>
#include <utility>

#include "ruvia/web/redis/redis.h"

#include "redis/redis_command_args.h"
#include "redis/redis_reply_parsers.h"

namespace ruvia {

task<std::optional<std::pmr::string>> redis_handle::hget(
    std::string_view key, std::string_view field) const {
    registration_.require_active();
    return execute_mapped<std::optional<std::pmr::string>>(pool(), detail::make_owned_redis_command(resource_, {"HGET", key, field}), detail::parse_redis_optional_string);
}

task<std::int64_t> redis_handle::hset(
    std::string_view key, std::string_view field, std::string_view value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"HSET", key, field, value}), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::hset(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields_value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::redis_hset_fields_args(key, fields_value, resource_), detail::parse_redis_integer);
}

task<std::pmr::vector<std::optional<std::pmr::string>>> redis_handle::hmget(
    std::string_view key, std::span<const std::string_view> fields_value) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::optional<std::pmr::string>>>(pool(), detail::redis_command_with_key_fields("HMGET", key, fields_value, resource_), detail::parse_redis_optional_string_array);
}

task<std::pmr::vector<redis_key_value>> redis_handle::hget_all(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<redis_key_value>>(pool(),
        detail::make_owned_redis_command(resource_, {"HGETALL", key}),
        [](redis_value&& reply, std::pmr::memory_resource* resource) {
            return detail::parse_redis_key_value_array(std::move(reply), resource, "unexpected redis hgetall reply");
        });
}

task<std::int64_t> redis_handle::hdel(
    std::string_view key, std::string_view field) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"HDEL", key, field}), detail::parse_redis_integer);
}

task<bool> redis_handle::hexists(std::string_view key, std::string_view field) const {
    registration_.require_active();
    return execute_mapped<bool>(pool(), detail::make_owned_redis_command(resource_, {"HEXISTS", key, field}), detail::parse_redis_integer_bool);
}

task<std::int64_t> redis_handle::hlen(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"HLEN", key}), detail::parse_redis_integer);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::hkeys(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(pool(), detail::make_owned_redis_command(resource_, {"HKEYS", key}), detail::parse_redis_string_array);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::hvals(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(pool(), detail::make_owned_redis_command(resource_, {"HVALS", key}), detail::parse_redis_string_array);
}

task<std::int64_t> redis_handle::hincr_by(
    std::string_view key, std::string_view field, std::int64_t value) const {
    registration_.require_active();
    std::array<char, detail::redis_int_scratch_size> value_scratch{};
    const std::string_view args[]{"HINCRBY", key, field, detail::format_redis_int(value_scratch, value)};
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, args), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::lpush(
    std::string_view key, std::string_view value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"LPUSH", key, value}), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::rpush(
    std::string_view key, std::string_view value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"RPUSH", key, value}), detail::parse_redis_integer);
}

task<std::optional<std::pmr::string>> redis_handle::lpop(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::optional<std::pmr::string>>(pool(), detail::make_owned_redis_command(resource_, {"LPOP", key}), detail::parse_redis_optional_string);
}

task<std::optional<std::pmr::string>> redis_handle::rpop(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::optional<std::pmr::string>>(pool(), detail::make_owned_redis_command(resource_, {"RPOP", key}), detail::parse_redis_optional_string);
}

task<std::int64_t> redis_handle::llen(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"LLEN", key}), detail::parse_redis_integer);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::lrange(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(pool(), detail::make_redis_range_command("LRANGE", key, start, stop, resource_), detail::parse_redis_string_array);
}

task<std::optional<std::pmr::string>> redis_handle::lindex(
    std::string_view key, std::int64_t index) const {
    registration_.require_active();
    return execute_mapped<std::optional<std::pmr::string>>(pool(), detail::make_redis_integer_command("LINDEX", key, index, resource_), detail::parse_redis_optional_string);
}

task<void> redis_handle::lset(
    std::string_view key, std::int64_t index, std::string_view value) const {
    registration_.require_active();
    std::array<char, detail::redis_int_scratch_size> index_scratch{};
    const std::string_view args[]{"LSET", key, detail::format_redis_int(index_scratch, index), value};
    return execute_mapped<void>(pool(), detail::make_owned_redis_command(resource_, args), detail::parse_redis_ok);
}

task<void> redis_handle::ltrim(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<void>(pool(), detail::make_redis_range_command("LTRIM", key, start, stop, resource_), detail::parse_redis_ok);
}

task<std::int64_t> redis_handle::lrem(
    std::string_view key, std::int64_t count, std::string_view value) const {
    registration_.require_active();
    std::array<char, detail::redis_int_scratch_size> count_scratch{};
    const std::string_view args[]{"LREM", key, detail::format_redis_int(count_scratch, count), value};
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, args), detail::parse_redis_integer);
}

}  // namespace ruvia
