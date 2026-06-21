#include "ruvia/redis/Redis.h"

#include "RedisHandleCommandOps.h"
#include "RedisHandleHelpers.h"
#include "RedisUtils.h"

#include <string_view>
#include <utility>

namespace ruvia {

Task<std::optional<std::pmr::string>> RedisHandle::hget(std::string_view key, std::string_view field) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"HGET", key, field}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::hset(std::string_view key, std::string_view field, std::string_view value) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"HSET", key, field, value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::hset(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fields) const {
    return detail::redisIntegerCommand(pool_, detail::redisHsetFieldsArgs(key, fields, resource_), resource_);
}

Task<std::int64_t> RedisHandle::hset(std::string_view key, std::initializer_list<std::pair<std::string_view, std::string_view>> fields) const {
    return hset(key, std::span<const std::pair<std::string_view, std::string_view>>(fields.begin(), fields.size()));
}

Task<std::pmr::vector<std::optional<std::pmr::string>>> RedisHandle::hmget(std::string_view key, std::span<const std::string_view> fields) const {
    return detail::redisOptionalStringArrayCommand(pool_, detail::redisCommandWithKeyFields("HMGET", key, fields, resource_), resource_);
}

Task<std::pmr::vector<std::optional<std::pmr::string>>> RedisHandle::hmget(std::string_view key, std::initializer_list<std::string_view> fields) const {
    return hmget(key, std::span<const std::string_view>(fields.begin(), fields.size()));
}

Task<std::pmr::vector<RedisKeyValue>> RedisHandle::hgetAll(std::string_view key) const {
    return detail::executeRedisKeyValueArray(
        pool_,
        detail::ownRedisArgs({"HGETALL", key}, resource_),
        "unexpected redis hgetall reply",
        resource_);
}

Task<std::int64_t> RedisHandle::hdel(std::string_view key, std::string_view field) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"HDEL", key, field}, resource_), resource_);
}

Task<bool> RedisHandle::hexists(std::string_view key, std::string_view field) const {
    return detail::executeRedisIntegerBool(pool_, detail::ownRedisArgs({"HEXISTS", key, field}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::hlen(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"HLEN", key}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::hkeys(std::string_view key) const {
    return detail::redisStringArrayCommand(pool_, detail::ownRedisArgs({"HKEYS", key}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::hvals(std::string_view key) const {
    return detail::redisStringArrayCommand(pool_, detail::ownRedisArgs({"HVALS", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::hincrBy(std::string_view key, std::string_view field, std::int64_t value) const {
    auto amount = detail::redisIntString(value, resource_);
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"HINCRBY", key, field, std::string_view(amount)}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::lpush(std::string_view key, std::string_view value) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"LPUSH", key, value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::rpush(std::string_view key, std::string_view value) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"RPUSH", key, value}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::lpop(std::string_view key) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"LPOP", key}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::rpop(std::string_view key) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"RPOP", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::llen(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"LLEN", key}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::lrange(std::string_view key, std::int64_t start, std::int64_t stop) const {
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return detail::redisStringArrayCommand(pool_, detail::ownRedisArgs({"LRANGE", key, std::string_view(startValue), std::string_view(stopValue)}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::lindex(std::string_view key, std::int64_t index) const {
    auto indexValue = detail::redisIntString(index, resource_);
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"LINDEX", key, std::string_view(indexValue)}, resource_), resource_);
}

Task<void> RedisHandle::lset(std::string_view key, std::int64_t index, std::string_view value) const {
    auto indexValue = detail::redisIntString(index, resource_);
    return detail::redisOkCommand(pool_, detail::ownRedisArgs({"LSET", key, std::string_view(indexValue), value}, resource_), resource_);
}

Task<void> RedisHandle::ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const {
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return detail::redisOkCommand(pool_, detail::ownRedisArgs({"LTRIM", key, std::string_view(startValue), std::string_view(stopValue)}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::lrem(std::string_view key, std::int64_t count, std::string_view value) const {
    auto countValue = detail::redisIntString(count, resource_);
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"LREM", key, std::string_view(countValue), value}, resource_), resource_);
}

}  // namespace ruvia
