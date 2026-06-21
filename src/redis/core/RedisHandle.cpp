#include "ruvia/redis/Redis.h"

#include "RedisHandleCommandOps.h"
#include "RedisHandleHelpers.h"
#include "../RedisInternal.h"
#include "RedisUtils.h"

#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace ruvia {

RedisHandle::RedisHandle(
    detail::RedisPool& pool,
    std::pmr::memory_resource* resource) noexcept
    : pool_(pool),
      resource_(detail::resolveRedisResource(resource)) {}

Task<RedisValue> RedisHandle::command(std::initializer_list<std::string_view> args) const {
    return detail::executeOwnedRedisCommand(pool_, detail::ownRedisArgs(args, resource_), resource_);
}

Task<RedisValue> RedisHandle::command(std::span<const std::string_view> args) const {
    return detail::executeOwnedRedisCommand(pool_, detail::ownRedisArgs(args, resource_), resource_);
}

Task<void> RedisHandle::ping() const {
    return detail::executeRedisPing(pool_, detail::ownRedisArgs({"PING"}, resource_), resource_);
}

Task<std::pmr::string> RedisHandle::ping(std::string_view message) const {
    return detail::redisStatusCommand(pool_, detail::ownRedisArgs({"PING", message}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::get(std::string_view key) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"GET", key}, resource_), resource_);
}

Task<std::pmr::vector<std::optional<std::pmr::string>>> RedisHandle::mget(std::span<const std::string_view> keys) const {
    return detail::redisOptionalStringArrayCommand(pool_, detail::redisCommandWithKeys("MGET", keys, resource_), resource_);
}

Task<std::pmr::vector<std::optional<std::pmr::string>>> RedisHandle::mget(std::initializer_list<std::string_view> keys) const {
    return mget(std::span<const std::string_view>(keys.begin(), keys.size()));
}

Task<void> RedisHandle::set(std::string_view key, std::string_view value) const {
    return detail::redisOkCommand(pool_, detail::ownRedisArgs({"SET", key, value}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::set(std::string_view key, std::string_view value, RedisSetOptions options) const {
    if (options.nx && options.xx) {
        throw std::invalid_argument("redis set options cannot combine NX and XX");
    }
    if (options.ttl.count() > 0 && options.keepTtl) {
        throw std::invalid_argument("redis set options cannot combine TTL and KEEPTTL");
    }
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(8);
    detail::emplaceRedisString(args, "SET");
    detail::emplaceRedisString(args, key);
    detail::emplaceRedisString(args, value);
    if (options.ttl.count() > 0) {
        auto ttlValue = detail::redisMillisecondsString(options.ttl, resource_);
        detail::emplaceRedisString(args, "PX");
        args.emplace_back(std::move(ttlValue));
    }
    if (options.nx) {
        detail::emplaceRedisString(args, "NX");
    }
    if (options.xx) {
        detail::emplaceRedisString(args, "XX");
    }
    if (options.get) {
        detail::emplaceRedisString(args, "GET");
    }
    if (options.keepTtl) {
        detail::emplaceRedisString(args, "KEEPTTL");
    }

    return detail::executeRedisSetWithOptions(pool_, std::move(args), options.get, resource_);
}

Task<void> RedisHandle::mset(std::span<const std::pair<std::string_view, std::string_view>> items) const {
    return detail::redisOkCommand(pool_, detail::redisMsetArgs(items, resource_), resource_);
}

Task<void> RedisHandle::mset(std::initializer_list<std::pair<std::string_view, std::string_view>> items) const {
    return mset(std::span<const std::pair<std::string_view, std::string_view>>(items.begin(), items.size()));
}

Task<void> RedisHandle::setEx(std::string_view key, std::chrono::seconds ttl, std::string_view value) const {
    auto ttlValue = detail::redisSecondsString(ttl, resource_);
    return detail::redisOkCommand(pool_, detail::ownRedisArgs({"SETEX", key, std::string_view(ttlValue), value}, resource_), resource_);
}

Task<bool> RedisHandle::setNx(std::string_view key, std::string_view value) const {
    return detail::executeRedisSetNx(pool_, detail::ownRedisArgs({"SET", key, value, "NX"}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::getDel(std::string_view key) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"GETDEL", key}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::getSet(std::string_view key, std::string_view value) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"GETSET", key, value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::append(std::string_view key, std::string_view value) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"APPEND", key, value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::strlen(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"STRLEN", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::incrBy(std::string_view key, std::int64_t value) const {
    auto amount = detail::redisIntString(value, resource_);
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"INCRBY", key, std::string_view(amount)}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::decr(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"DECR", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::decrBy(std::string_view key, std::int64_t value) const {
    auto amount = detail::redisIntString(value, resource_);
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"DECRBY", key, std::string_view(amount)}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::del(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"DEL", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::unlink(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"UNLINK", key}, resource_), resource_);
}

Task<bool> RedisHandle::exists(std::string_view key) const {
    return detail::executeRedisIntegerBool(pool_, detail::ownRedisArgs({"EXISTS", key}, resource_), resource_);
}

Task<bool> RedisHandle::touch(std::string_view key) const {
    return detail::executeRedisIntegerBool(pool_, detail::ownRedisArgs({"TOUCH", key}, resource_), resource_);
}

Task<std::pmr::string> RedisHandle::type(std::string_view key) const {
    return detail::redisStatusCommand(pool_, detail::ownRedisArgs({"TYPE", key}, resource_), resource_);
}

Task<void> RedisHandle::rename(std::string_view key, std::string_view newKey) const {
    return detail::redisOkCommand(pool_, detail::ownRedisArgs({"RENAME", key, newKey}, resource_), resource_);
}

Task<bool> RedisHandle::renameNx(std::string_view key, std::string_view newKey) const {
    return detail::executeRedisIntegerBool(pool_, detail::ownRedisArgs({"RENAMENX", key, newKey}, resource_), resource_);
}

Task<bool> RedisHandle::expire(std::string_view key, std::chrono::seconds ttl) const {
    auto ttlValue = detail::redisSecondsString(ttl, resource_);
    return detail::executeRedisIntegerBool(pool_, detail::ownRedisArgs({"EXPIRE", key, std::string_view(ttlValue)}, resource_), resource_);
}

Task<bool> RedisHandle::expireAt(std::string_view key, std::chrono::seconds unixTime) const {
    auto value = detail::redisSecondsString(unixTime, resource_);
    return detail::executeRedisIntegerBool(pool_, detail::ownRedisArgs({"EXPIREAT", key, std::string_view(value)}, resource_), resource_);
}

Task<bool> RedisHandle::persist(std::string_view key) const {
    return detail::executeRedisIntegerBool(pool_, detail::ownRedisArgs({"PERSIST", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::ttl(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"TTL", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::pttl(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"PTTL", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::incr(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"INCR", key}, resource_), resource_);
}

}  // namespace ruvia
