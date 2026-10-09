#include <string_view>

#include "ruvia/web/detail/redis/redis_utils.h"
#include "ruvia/web/redis/redis.h"

#include "redis/redis_handle_command_ops.h"
#include "redis/redis_handle_helpers.h"

namespace ruvia {

scoped_operation<std::int64_t> redis_handle::sadd(
    std::string_view key, std::string_view member) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"SADD", key, member}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::srem(
    std::string_view key, std::string_view member) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"SREM", key, member}, resource_), resource_));
}

scoped_operation<std::pmr::vector<std::pmr::string>> redis_handle::smembers(
    std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_string_array_command(
        executor(), detail::own_redis_args({"SMEMBERS", key}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::scard(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"SCARD", key}, resource_), resource_));
}

scoped_operation<bool> redis_handle::sismember(std::string_view key, std::string_view member) const {
    registration_.require_active();
    return scoped(detail::execute_redis_integer_bool(
        executor(), detail::own_redis_args({"SISMEMBER", key, member}, resource_), resource_));
}

scoped_operation<std::optional<std::pmr::string>> redis_handle::spop(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_string_command(
        executor(), detail::own_redis_args({"SPOP", key}, resource_), resource_));
}

scoped_operation<std::optional<std::pmr::string>> redis_handle::srand_member(
    std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_string_command(
        executor(), detail::own_redis_args({"SRANDMEMBER", key}, resource_), resource_));
}

scoped_operation<std::pmr::vector<std::pmr::string>> redis_handle::sinter(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return scoped(detail::redis_string_array_command(
        executor(), detail::redis_command_with_keys("SINTER", keys, resource_), resource_));
}

scoped_operation<std::pmr::vector<std::pmr::string>> redis_handle::sunion(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return scoped(detail::redis_string_array_command(
        executor(), detail::redis_command_with_keys("SUNION", keys, resource_), resource_));
}

scoped_operation<std::pmr::vector<std::pmr::string>> redis_handle::sdiff(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return scoped(detail::redis_string_array_command(
        executor(), detail::redis_command_with_keys("SDIFF", keys, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::zadd(
    std::string_view key, double score, std::string_view member) const {
    registration_.require_active();
    auto score_value = detail::redis_score_string(score, resource_);
    return scoped(detail::redis_integer_command(executor(),
        detail::own_redis_args({"ZADD", key, std::string_view(score_value), member}, resource_),
        resource_));
}

scoped_operation<std::int64_t> redis_handle::zrem(
    std::string_view key, std::string_view member) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"ZREM", key, member}, resource_), resource_));
}

scoped_operation<std::pmr::vector<std::pmr::string>> redis_handle::zrange(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    auto start_value = detail::redis_int_string(start, resource_);
    auto stop_value = detail::redis_int_string(stop, resource_);
    return scoped(detail::redis_string_array_command(executor(),
        detail::own_redis_args(
            {"ZRANGE", key, std::string_view(start_value), std::string_view(stop_value)}, resource_),
        resource_));
}

scoped_operation<std::pmr::vector<redis_scored_value>> redis_handle::zrange_with_scores(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    auto start_value = detail::redis_int_string(start, resource_);
    auto stop_value = detail::redis_int_string(stop, resource_);
    return scoped(detail::execute_redis_scored_array(executor(),
        detail::own_redis_args({"ZRANGE", key, std::string_view(start_value),
                                   std::string_view(stop_value), "WITHSCORES"},
            resource_),
        resource_));
}

scoped_operation<std::optional<double>> redis_handle::zscore(
    std::string_view key, std::string_view member) const {
    registration_.require_active();
    return scoped(detail::execute_redis_optional_double(executor(),
        detail::own_redis_args({"ZSCORE", key, member}, resource_), "invalid redis zscore reply",
        resource_));
}

scoped_operation<std::int64_t> redis_handle::zcard(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"ZCARD", key}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::zcount(
    std::string_view key, double min_value, double max_value) const {
    registration_.require_active();
    auto min_text = detail::redis_score_string(min_value, resource_);
    auto max_text = detail::redis_score_string(max_value, resource_);
    return scoped(detail::redis_integer_command(executor(),
        detail::own_redis_args(
            {"ZCOUNT", key, std::string_view(min_text), std::string_view(max_text)}, resource_),
        resource_));
}

}  // namespace ruvia
