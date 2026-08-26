#include "ruvia/web/redis/Redis.h"

#include "ruvia/web/detail/redis/RedisHandleCommandOps.h"
#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <string_view>
#include <utility>

namespace ruvia {

ScopedOperation<std::optional<std::pmr::string>> RedisHandle::hget(
    std::string_view key, std::string_view field) const {
    requireActive();
    return scoped(detail::redisStringCommand(
        executor(), detail::ownRedisArgs({"HGET", key, field}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::hset(
    std::string_view key, std::string_view field, std::string_view value) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"HSET", key, field, value}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::hset(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::redisHsetFieldsArgs(key, fields, resource_), resource_));
}

ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> RedisHandle::hmget(
    std::string_view key, std::span<const std::string_view> fields) const {
    requireActive();
    return scoped(detail::redisOptionalStringArrayCommand(
        executor(), detail::redisCommandWithKeyFields("HMGET", key, fields, resource_), resource_));
}

ScopedOperation<std::pmr::vector<RedisKeyValue>> RedisHandle::hgetAll(std::string_view key) const {
    requireActive();
    return scoped(detail::executeRedisKeyValueArray(executor(),
        detail::ownRedisArgs({"HGETALL", key}, resource_), "unexpected redis hgetall reply",
        resource_));
}

ScopedOperation<std::int64_t> RedisHandle::hdel(
    std::string_view key, std::string_view field) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"HDEL", key, field}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::hexists(std::string_view key, std::string_view field) const {
    requireActive();
    return scoped(detail::executeRedisIntegerBool(
        executor(), detail::ownRedisArgs({"HEXISTS", key, field}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::hlen(std::string_view key) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"HLEN", key}, resource_), resource_));
}

ScopedOperation<std::pmr::vector<std::pmr::string>> RedisHandle::hkeys(std::string_view key) const {
    requireActive();
    return scoped(detail::redisStringArrayCommand(
        executor(), detail::ownRedisArgs({"HKEYS", key}, resource_), resource_));
}

ScopedOperation<std::pmr::vector<std::pmr::string>> RedisHandle::hvals(std::string_view key) const {
    requireActive();
    return scoped(detail::redisStringArrayCommand(
        executor(), detail::ownRedisArgs({"HVALS", key}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::hincrBy(
    std::string_view key, std::string_view field, std::int64_t value) const {
    requireActive();
    auto amount = detail::redisIntString(value, resource_);
    return scoped(detail::redisIntegerCommand(executor(),
        detail::ownRedisArgs({"HINCRBY", key, field, std::string_view(amount)}, resource_),
        resource_));
}

ScopedOperation<std::int64_t> RedisHandle::lpush(
    std::string_view key, std::string_view value) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"LPUSH", key, value}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::rpush(
    std::string_view key, std::string_view value) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"RPUSH", key, value}, resource_), resource_));
}

ScopedOperation<std::optional<std::pmr::string>> RedisHandle::lpop(std::string_view key) const {
    requireActive();
    return scoped(detail::redisStringCommand(
        executor(), detail::ownRedisArgs({"LPOP", key}, resource_), resource_));
}

ScopedOperation<std::optional<std::pmr::string>> RedisHandle::rpop(std::string_view key) const {
    requireActive();
    return scoped(detail::redisStringCommand(
        executor(), detail::ownRedisArgs({"RPOP", key}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::llen(std::string_view key) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"LLEN", key}, resource_), resource_));
}

ScopedOperation<std::pmr::vector<std::pmr::string>> RedisHandle::lrange(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    requireActive();
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return scoped(detail::redisStringArrayCommand(executor(),
        detail::ownRedisArgs(
            {"LRANGE", key, std::string_view(startValue), std::string_view(stopValue)}, resource_),
        resource_));
}

ScopedOperation<std::optional<std::pmr::string>> RedisHandle::lindex(
    std::string_view key, std::int64_t index) const {
    requireActive();
    auto indexValue = detail::redisIntString(index, resource_);
    return scoped(detail::redisStringCommand(executor(),
        detail::ownRedisArgs({"LINDEX", key, std::string_view(indexValue)}, resource_), resource_));
}

ScopedOperation<void> RedisHandle::lset(
    std::string_view key, std::int64_t index, std::string_view value) const {
    requireActive();
    auto indexValue = detail::redisIntString(index, resource_);
    return scoped(detail::redisOkCommand(executor(),
        detail::ownRedisArgs({"LSET", key, std::string_view(indexValue), value}, resource_),
        resource_));
}

ScopedOperation<void> RedisHandle::ltrim(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    requireActive();
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return scoped(detail::redisOkCommand(executor(),
        detail::ownRedisArgs(
            {"LTRIM", key, std::string_view(startValue), std::string_view(stopValue)}, resource_),
        resource_));
}

ScopedOperation<std::int64_t> RedisHandle::lrem(
    std::string_view key, std::int64_t count, std::string_view value) const {
    requireActive();
    auto countValue = detail::redisIntString(count, resource_);
    return scoped(detail::redisIntegerCommand(executor(),
        detail::ownRedisArgs({"LREM", key, std::string_view(countValue), value}, resource_),
        resource_));
}

}  // namespace ruvia
