#include "ruvia/web/redis/Redis.h"

#include "ruvia/web/detail/redis/RedisHandleCommandOps.h"
#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisTypesAccess.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <memory_resource>
#include <string_view>
#include <utility>

namespace ruvia {

namespace {

Task<RedisTtl> redisTtlCommand(detail::RedisPool& pool, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource, bool secondsPrecision) {
    const auto value = co_await detail::redisIntegerCommand(pool, std::move(args), resource);
    if (value == -2) {
        co_return detail::RedisTypesAccess::ttl(RedisTtlState::kMissing);
    }
    if (value == -1) {
        co_return detail::RedisTypesAccess::ttl(RedisTtlState::kPersistent);
    }
    if (value < 0) {
        throw RedisError(RedisError::Code::kProtocolError, "invalid redis TTL reply");
    }

    using Milliseconds = std::chrono::milliseconds;
    auto milliseconds = value;
    if (secondsPrecision) {
        constexpr auto kScale = Milliseconds(std::chrono::seconds(1)).count();
        if (value > Milliseconds::max().count() / kScale) {
            throw RedisError(RedisError::Code::kProtocolError, "redis TTL reply exceeds milliseconds range");
        }
        milliseconds *= kScale;
    }
    co_return detail::RedisTypesAccess::ttl(RedisTtlState::kExpiring, Milliseconds(milliseconds));
}

}  // namespace

RedisHandle::RedisHandle(detail::RedisPool& pool, std::pmr::memory_resource* resource, detail::ScopedOperationScope& operationScope) noexcept
    : detail::ScopedCapabilityNode(operationScope, &RedisHandle::expireCapability),
      pool_(&pool),
      resource_(detail::pmrResourceOrDefault(resource)) {}

RedisHandle::RedisHandle(const RedisHandle& other) noexcept
    : detail::ScopedCapabilityNode(other),
      pool_(other.pool_),
      resource_(other.resource_) {}

void RedisHandle::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    auto& handle = static_cast<RedisHandle&>(capability);
    handle.pool_ = nullptr;
    handle.resource_ = nullptr;
}

ScopedOperation<RedisValue> RedisHandle::command(std::span<const std::string_view> args) const {
    requireActive();
    return scoped(detail::executeOwnedRedisCommand(*pool_, detail::ownRedisArgs(args, resource_), resource_));
}

ScopedOperation<void> RedisHandle::ping() const {
    requireActive();
    return scoped(detail::executeRedisPing(*pool_, detail::ownRedisArgs({"PING"}, resource_), resource_));
}

ScopedOperation<std::pmr::string> RedisHandle::ping(std::string_view message) const {
    requireActive();
    return scoped(detail::redisStatusCommand(*pool_, detail::ownRedisArgs({"PING", message}, resource_), resource_));
}

ScopedOperation<std::optional<std::pmr::string>> RedisHandle::get(std::string_view key) const {
    requireActive();
    return scoped(detail::redisStringCommand(*pool_, detail::ownRedisArgs({"GET", key}, resource_), resource_));
}

ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> RedisHandle::mget(std::span<const std::string_view> keys) const {
    requireActive();
    return scoped(detail::redisOptionalStringArrayCommand(*pool_, detail::redisCommandWithKeys("MGET", keys, resource_), resource_));
}

ScopedOperation<void> RedisHandle::set(std::string_view key, std::string_view value) const {
    requireActive();
    return scoped(detail::redisOkCommand(*pool_, detail::ownRedisArgs({"SET", key, value}, resource_), resource_));
}

ScopedOperation<std::optional<std::pmr::string>> RedisHandle::set(std::string_view key, std::string_view value, RedisSetOptions options) const {
    requireActive();
    auto args = detail::redisSetArgs(key, value, options, resource_);
    return scoped(detail::executeRedisSetWithOptions(*pool_, std::move(args), options.returnPrevious, resource_));
}

ScopedOperation<void> RedisHandle::mset(std::span<const std::pair<std::string_view, std::string_view>> items) const {
    requireActive();
    return scoped(detail::redisOkCommand(*pool_, detail::redisMsetArgs(items, resource_), resource_));
}

ScopedOperation<void> RedisHandle::setEx(std::string_view key, std::chrono::seconds ttl, std::string_view value) const {
    requireActive();
    auto ttlValue = detail::redisSecondsString(ttl, resource_);
    return scoped(detail::redisOkCommand(*pool_, detail::ownRedisArgs({"SETEX", key, std::string_view(ttlValue), value}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::setNx(std::string_view key, std::string_view value) const {
    requireActive();
    return scoped(detail::executeRedisSetNx(*pool_, detail::ownRedisArgs({"SET", key, value, "NX"}, resource_), resource_));
}

ScopedOperation<std::optional<std::pmr::string>> RedisHandle::getDel(std::string_view key) const {
    requireActive();
    return scoped(detail::redisStringCommand(*pool_, detail::ownRedisArgs({"GETDEL", key}, resource_), resource_));
}

ScopedOperation<std::optional<std::pmr::string>> RedisHandle::getSet(std::string_view key, std::string_view value) const {
    requireActive();
    return scoped(detail::redisStringCommand(*pool_, detail::ownRedisArgs({"GETSET", key, value}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::append(std::string_view key, std::string_view value) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(*pool_, detail::ownRedisArgs({"APPEND", key, value}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::strlen(std::string_view key) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(*pool_, detail::ownRedisArgs({"STRLEN", key}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::incrBy(std::string_view key, std::int64_t value) const {
    requireActive();
    auto amount = detail::redisIntString(value, resource_);
    return scoped(detail::redisIntegerCommand(*pool_, detail::ownRedisArgs({"INCRBY", key, std::string_view(amount)}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::decr(std::string_view key) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(*pool_, detail::ownRedisArgs({"DECR", key}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::decrBy(std::string_view key, std::int64_t value) const {
    requireActive();
    auto amount = detail::redisIntString(value, resource_);
    return scoped(detail::redisIntegerCommand(*pool_, detail::ownRedisArgs({"DECRBY", key, std::string_view(amount)}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::del(std::string_view key) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(*pool_, detail::ownRedisArgs({"DEL", key}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::unlink(std::string_view key) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(*pool_, detail::ownRedisArgs({"UNLINK", key}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::exists(std::string_view key) const {
    requireActive();
    return scoped(detail::executeRedisIntegerBool(*pool_, detail::ownRedisArgs({"EXISTS", key}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::touch(std::string_view key) const {
    requireActive();
    return scoped(detail::executeRedisIntegerBool(*pool_, detail::ownRedisArgs({"TOUCH", key}, resource_), resource_));
}

ScopedOperation<std::pmr::string> RedisHandle::type(std::string_view key) const {
    requireActive();
    return scoped(detail::redisStatusCommand(*pool_, detail::ownRedisArgs({"TYPE", key}, resource_), resource_));
}

ScopedOperation<void> RedisHandle::rename(std::string_view key, std::string_view newKey) const {
    requireActive();
    return scoped(detail::redisOkCommand(*pool_, detail::ownRedisArgs({"RENAME", key, newKey}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::renameNx(std::string_view key, std::string_view newKey) const {
    requireActive();
    return scoped(detail::executeRedisIntegerBool(*pool_, detail::ownRedisArgs({"RENAMENX", key, newKey}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::expire(std::string_view key, std::chrono::seconds ttl) const {
    requireActive();
    auto ttlValue = detail::redisSecondsString(ttl, resource_);
    return scoped(detail::executeRedisIntegerBool(*pool_, detail::ownRedisArgs({"EXPIRE", key, std::string_view(ttlValue)}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::expireAt(std::string_view key, std::chrono::system_clock::time_point expiresAt) const {
    requireActive();
    auto value = detail::redisSecondsString(std::chrono::floor<std::chrono::seconds>(expiresAt.time_since_epoch()), resource_);
    return scoped(detail::executeRedisIntegerBool(*pool_, detail::ownRedisArgs({"EXPIREAT", key, std::string_view(value)}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::persist(std::string_view key) const {
    requireActive();
    return scoped(detail::executeRedisIntegerBool(*pool_, detail::ownRedisArgs({"PERSIST", key}, resource_), resource_));
}

ScopedOperation<RedisTtl> RedisHandle::ttl(std::string_view key) const {
    requireActive();
    return scoped(redisTtlCommand(*pool_, detail::ownRedisArgs({"TTL", key}, resource_), resource_, true));
}

ScopedOperation<RedisTtl> RedisHandle::pttl(std::string_view key) const {
    requireActive();
    return scoped(redisTtlCommand(*pool_, detail::ownRedisArgs({"PTTL", key}, resource_), resource_, false));
}

ScopedOperation<std::int64_t> RedisHandle::incr(std::string_view key) const {
    requireActive();
    return scoped(detail::redisIntegerCommand(*pool_, detail::ownRedisArgs({"INCR", key}, resource_), resource_));
}

}  // namespace ruvia
