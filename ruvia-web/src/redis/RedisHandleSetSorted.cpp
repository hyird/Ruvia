#include "ruvia/web/redis/Redis.h"

#include "ruvia/web/detail/redis/RedisHandleCommandOps.h"
#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <string_view>

namespace ruvia {

ScopedOperation<std::int64_t> RedisHandle::sadd(
    std::string_view key, std::string_view member) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"SADD", key, member}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::srem(
    std::string_view key, std::string_view member) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"SREM", key, member}, resource_), resource_));
}

ScopedOperation<std::pmr::vector<std::pmr::string>> RedisHandle::smembers(
    std::string_view key) const {
    requireActive();
    return scoped(detail::redisStringArrayCommand(
        executor(), detail::ownRedisArgs({"SMEMBERS", key}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::scard(std::string_view key) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"SCARD", key}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::sismember(std::string_view key, std::string_view member) const {
    requireActive();
    return scoped(detail::executeRedisIntegerBool(
        executor(), detail::ownRedisArgs({"SISMEMBER", key, member}, resource_), resource_));
}

ScopedOperation<std::optional<std::pmr::string>> RedisHandle::spop(std::string_view key) const {
    requireActive();
    return scoped(detail::redisStringCommand(
        executor(), detail::ownRedisArgs({"SPOP", key}, resource_), resource_));
}

ScopedOperation<std::optional<std::pmr::string>> RedisHandle::srandMember(
    std::string_view key) const {
    requireActive();
    return scoped(detail::redisStringCommand(
        executor(), detail::ownRedisArgs({"SRANDMEMBER", key}, resource_), resource_));
}

ScopedOperation<std::pmr::vector<std::pmr::string>> RedisHandle::sinter(
    std::span<const std::string_view> keys) const {
    requireActive();
    return scoped(detail::redisStringArrayCommand(
        executor(), detail::redisCommandWithKeys("SINTER", keys, resource_), resource_));
}

ScopedOperation<std::pmr::vector<std::pmr::string>> RedisHandle::sunion(
    std::span<const std::string_view> keys) const {
    requireActive();
    return scoped(detail::redisStringArrayCommand(
        executor(), detail::redisCommandWithKeys("SUNION", keys, resource_), resource_));
}

ScopedOperation<std::pmr::vector<std::pmr::string>> RedisHandle::sdiff(
    std::span<const std::string_view> keys) const {
    requireActive();
    return scoped(detail::redisStringArrayCommand(
        executor(), detail::redisCommandWithKeys("SDIFF", keys, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::zadd(
    std::string_view key, double score, std::string_view member) const {
    requireActive();
    auto scoreValue = detail::redisScoreString(score, resource_);
    return scoped(detail::redisIntegerCommand(executor(),
        detail::ownRedisArgs({"ZADD", key, std::string_view(scoreValue), member}, resource_),
        resource_));
}

ScopedOperation<std::int64_t> RedisHandle::zrem(
    std::string_view key, std::string_view member) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"ZREM", key, member}, resource_), resource_));
}

ScopedOperation<std::pmr::vector<std::pmr::string>> RedisHandle::zrange(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    requireActive();
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return scoped(detail::redisStringArrayCommand(executor(),
        detail::ownRedisArgs(
            {"ZRANGE", key, std::string_view(startValue), std::string_view(stopValue)}, resource_),
        resource_));
}

ScopedOperation<std::pmr::vector<RedisScoredValue>> RedisHandle::zrangeWithScores(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    requireActive();
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return scoped(detail::executeRedisScoredArray(executor(),
        detail::ownRedisArgs({"ZRANGE", key, std::string_view(startValue),
                                 std::string_view(stopValue), "WITHSCORES"},
            resource_),
        resource_));
}

ScopedOperation<std::optional<double>> RedisHandle::zscore(
    std::string_view key, std::string_view member) const {
    requireActive();
    return scoped(detail::executeRedisOptionalDouble(executor(),
        detail::ownRedisArgs({"ZSCORE", key, member}, resource_), "invalid redis zscore reply",
        resource_));
}

ScopedOperation<std::int64_t> RedisHandle::zcard(std::string_view key) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"ZCARD", key}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::zcount(
    std::string_view key, double min, double max) const {
    requireActive();
    auto minValue = detail::redisScoreString(min, resource_);
    auto maxValue = detail::redisScoreString(max, resource_);
    return scoped(detail::redisIntegerCommand(executor(),
        detail::ownRedisArgs(
            {"ZCOUNT", key, std::string_view(minValue), std::string_view(maxValue)}, resource_),
        resource_));
}

}  // namespace ruvia
