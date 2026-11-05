#include <array>
#include <string_view>
#include <utility>

#include "ruvia/web/redis/redis.h"

#include "redis/redis_command_args.h"
#include "redis/redis_reply_parsers.h"

namespace ruvia {

task<std::int64_t> redis_handle::sadd(
    std::string_view key, std::string_view member) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"SADD", key, member}), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::srem(
    std::string_view key, std::string_view member) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"SREM", key, member}), detail::parse_redis_integer);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::smembers(
    std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(pool(), detail::make_owned_redis_command(resource_, {"SMEMBERS", key}), detail::parse_redis_string_array);
}

task<std::int64_t> redis_handle::scard(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"SCARD", key}), detail::parse_redis_integer);
}

task<bool> redis_handle::sismember(std::string_view key, std::string_view member) const {
    registration_.require_active();
    return execute_mapped<bool>(pool(), detail::make_owned_redis_command(resource_, {"SISMEMBER", key, member}), detail::parse_redis_integer_bool);
}

task<std::optional<std::pmr::string>> redis_handle::spop(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::optional<std::pmr::string>>(pool(), detail::make_owned_redis_command(resource_, {"SPOP", key}), detail::parse_redis_optional_string);
}

task<std::optional<std::pmr::string>> redis_handle::srand_member(
    std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::optional<std::pmr::string>>(pool(), detail::make_owned_redis_command(resource_, {"SRANDMEMBER", key}), detail::parse_redis_optional_string);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::sinter(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(pool(), detail::redis_command_with_keys("SINTER", keys, resource_), detail::parse_redis_string_array);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::sunion(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(pool(), detail::redis_command_with_keys("SUNION", keys, resource_), detail::parse_redis_string_array);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::sdiff(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(pool(), detail::redis_command_with_keys("SDIFF", keys, resource_), detail::parse_redis_string_array);
}

task<std::int64_t> redis_handle::zadd(
    std::string_view key, double score, std::string_view member) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_redis_zadd_command(key, score, member, resource_), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::zrem(
    std::string_view key, std::string_view member) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"ZREM", key, member}), detail::parse_redis_integer);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::zrange(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(pool(), detail::make_redis_range_command("ZRANGE", key, start, stop, resource_), detail::parse_redis_string_array);
}

task<std::pmr::vector<redis_scored_value>> redis_handle::zrange_with_scores(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    auto command = detail::make_redis_range_command("ZRANGE", key, start, stop, resource_, true);
    return execute_mapped<std::pmr::vector<redis_scored_value>>(pool(), std::move(command), detail::parse_redis_scored_array);
}

task<std::optional<double>> redis_handle::zscore(
    std::string_view key, std::string_view member) const {
    registration_.require_active();
    return execute_mapped<std::optional<double>>(pool(),
        detail::make_owned_redis_command(resource_, {"ZSCORE", key, member}),
        [](redis_value&& reply, std::pmr::memory_resource*) {
            return detail::parse_redis_optional_double(std::move(reply), "invalid redis zscore reply");
        });
}

task<std::int64_t> redis_handle::zcard(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"ZCARD", key}), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::zcount(
    std::string_view key, double min_value, double max_value) const {
    registration_.require_active();
    std::array<char, detail::redis_score_scratch_size> min_scratch{};
    std::array<char, detail::redis_score_scratch_size> max_scratch{};
    const std::string_view args[]{
        "ZCOUNT",
        key,
        detail::format_redis_score(min_scratch, min_value),
        detail::format_redis_score(max_scratch, max_value),
    };
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, args), detail::parse_redis_integer);
}

}  // namespace ruvia
