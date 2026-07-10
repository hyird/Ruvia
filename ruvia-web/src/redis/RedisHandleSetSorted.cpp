#include "ruvia/web/redis/Redis.h"

#include "ruvia/web/detail/redis/RedisHandleCommandOps.h"
#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <string_view>

namespace ruvia {

Task<std::int64_t> RedisHandle::sadd(std::string_view key, std::string_view member) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"SADD", key, member}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::srem(std::string_view key, std::string_view member) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"SREM", key, member}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::smembers(std::string_view key) const {
    return detail::redisStringArrayCommand(pool_, detail::ownRedisArgs({"SMEMBERS", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::scard(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"SCARD", key}, resource_), resource_);
}

Task<bool> RedisHandle::sismember(std::string_view key, std::string_view member) const {
    return detail::executeRedisIntegerBool(pool_, detail::ownRedisArgs({"SISMEMBER", key, member}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::spop(std::string_view key) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"SPOP", key}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::srandMember(std::string_view key) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"SRANDMEMBER", key}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::sinter(std::span<const std::string_view> keys) const {
    return detail::redisStringArrayCommand(pool_, detail::redisCommandWithKeys("SINTER", keys, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::sunion(std::span<const std::string_view> keys) const {
    return detail::redisStringArrayCommand(pool_, detail::redisCommandWithKeys("SUNION", keys, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::sdiff(std::span<const std::string_view> keys) const {
    return detail::redisStringArrayCommand(pool_, detail::redisCommandWithKeys("SDIFF", keys, resource_), resource_);
}

Task<std::int64_t> RedisHandle::zadd(std::string_view key, double score, std::string_view member) const {
    auto scoreValue = detail::redisScoreString(score, resource_);
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"ZADD", key, std::string_view(scoreValue), member}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::zrem(std::string_view key, std::string_view member) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"ZREM", key, member}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::zrange(std::string_view key, std::int64_t start, std::int64_t stop) const {
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return detail::redisStringArrayCommand(
        pool_,
        detail::ownRedisArgs({"ZRANGE", key, std::string_view(startValue), std::string_view(stopValue)}, resource_),
        resource_);
}

Task<std::pmr::vector<RedisScoredValue>> RedisHandle::zrangeWithScores(std::string_view key, std::int64_t start, std::int64_t stop) const {
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return detail::executeRedisScoredArray(
        pool_,
        detail::ownRedisArgs({"ZRANGE", key, std::string_view(startValue), std::string_view(stopValue), "WITHSCORES"}, resource_),
        resource_);
}

Task<std::optional<double>> RedisHandle::zscore(std::string_view key, std::string_view member) const {
    return detail::executeRedisOptionalDouble(
        pool_,
        detail::ownRedisArgs({"ZSCORE", key, member}, resource_),
        "invalid redis zscore reply",
        resource_);
}

Task<std::int64_t> RedisHandle::zcard(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"ZCARD", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::zcount(std::string_view key, double min, double max) const {
    auto minValue = detail::redisScoreString(min, resource_);
    auto maxValue = detail::redisScoreString(max, resource_);
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"ZCOUNT", key, std::string_view(minValue), std::string_view(maxValue)}, resource_), resource_);
}

}  // namespace ruvia
