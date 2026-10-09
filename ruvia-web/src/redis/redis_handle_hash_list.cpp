#include <string_view>
#include <utility>

#include "ruvia/web/detail/redis/redis_utils.h"
#include "ruvia/web/redis/redis.h"

#include "redis/redis_handle_command_ops.h"
#include "redis/redis_handle_helpers.h"

namespace ruvia {

scoped_operation<std::optional<std::pmr::string>> redis_handle::hget(
    std::string_view key, std::string_view field) const {
    registration_.require_active();
    return scoped(detail::redis_string_command(
        executor(), detail::own_redis_args({"HGET", key, field}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::hset(
    std::string_view key, std::string_view field, std::string_view value) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"HSET", key, field, value}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::hset(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields_value) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::redis_hset_fields_args(key, fields_value, resource_), resource_));
}

scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> redis_handle::hmget(
    std::string_view key, std::span<const std::string_view> fields_value) const {
    registration_.require_active();
    return scoped(detail::redis_optional_string_array_command(
        executor(), detail::redis_command_with_key_fields("HMGET", key, fields_value, resource_), resource_));
}

scoped_operation<std::pmr::vector<redis_key_value>> redis_handle::hget_all(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::execute_redis_key_value_array(executor(),
        detail::own_redis_args({"HGETALL", key}, resource_), "unexpected redis hgetall reply",
        resource_));
}

scoped_operation<std::int64_t> redis_handle::hdel(
    std::string_view key, std::string_view field) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"HDEL", key, field}, resource_), resource_));
}

scoped_operation<bool> redis_handle::hexists(std::string_view key, std::string_view field) const {
    registration_.require_active();
    return scoped(detail::execute_redis_integer_bool(
        executor(), detail::own_redis_args({"HEXISTS", key, field}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::hlen(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"HLEN", key}, resource_), resource_));
}

scoped_operation<std::pmr::vector<std::pmr::string>> redis_handle::hkeys(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_string_array_command(
        executor(), detail::own_redis_args({"HKEYS", key}, resource_), resource_));
}

scoped_operation<std::pmr::vector<std::pmr::string>> redis_handle::hvals(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_string_array_command(
        executor(), detail::own_redis_args({"HVALS", key}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::hincr_by(
    std::string_view key, std::string_view field, std::int64_t value) const {
    registration_.require_active();
    auto amount = detail::redis_int_string(value, resource_);
    return scoped(detail::redis_integer_command(executor(),
        detail::own_redis_args({"HINCRBY", key, field, std::string_view(amount)}, resource_),
        resource_));
}

scoped_operation<std::int64_t> redis_handle::lpush(
    std::string_view key, std::string_view value) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"LPUSH", key, value}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::rpush(
    std::string_view key, std::string_view value) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"RPUSH", key, value}, resource_), resource_));
}

scoped_operation<std::optional<std::pmr::string>> redis_handle::lpop(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_string_command(
        executor(), detail::own_redis_args({"LPOP", key}, resource_), resource_));
}

scoped_operation<std::optional<std::pmr::string>> redis_handle::rpop(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_string_command(
        executor(), detail::own_redis_args({"RPOP", key}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::llen(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"LLEN", key}, resource_), resource_));
}

scoped_operation<std::pmr::vector<std::pmr::string>> redis_handle::lrange(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    auto start_value = detail::redis_int_string(start, resource_);
    auto stop_value = detail::redis_int_string(stop, resource_);
    return scoped(detail::redis_string_array_command(executor(),
        detail::own_redis_args(
            {"LRANGE", key, std::string_view(start_value), std::string_view(stop_value)}, resource_),
        resource_));
}

scoped_operation<std::optional<std::pmr::string>> redis_handle::lindex(
    std::string_view key, std::int64_t index) const {
    registration_.require_active();
    auto index_value = detail::redis_int_string(index, resource_);
    return scoped(detail::redis_string_command(executor(),
        detail::own_redis_args({"LINDEX", key, std::string_view(index_value)}, resource_), resource_));
}

scoped_operation<void> redis_handle::lset(
    std::string_view key, std::int64_t index, std::string_view value) const {
    registration_.require_active();
    auto index_value = detail::redis_int_string(index, resource_);
    return scoped(detail::redis_ok_command(executor(),
        detail::own_redis_args({"LSET", key, std::string_view(index_value), value}, resource_),
        resource_));
}

scoped_operation<void> redis_handle::ltrim(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    auto start_value = detail::redis_int_string(start, resource_);
    auto stop_value = detail::redis_int_string(stop, resource_);
    return scoped(detail::redis_ok_command(executor(),
        detail::own_redis_args(
            {"LTRIM", key, std::string_view(start_value), std::string_view(stop_value)}, resource_),
        resource_));
}

scoped_operation<std::int64_t> redis_handle::lrem(
    std::string_view key, std::int64_t count, std::string_view value) const {
    registration_.require_active();
    auto count_value = detail::redis_int_string(count, resource_);
    return scoped(detail::redis_integer_command(executor(),
        detail::own_redis_args({"LREM", key, std::string_view(count_value), value}, resource_),
        resource_));
}

}  // namespace ruvia
