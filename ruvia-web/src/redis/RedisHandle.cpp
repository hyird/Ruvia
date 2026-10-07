#include <chrono>
#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/redis/RedisHandleCommandOps.h"
#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisTypesAccess.h"
#include "ruvia/web/detail/redis/RedisUtils.h"
#include "ruvia/web/redis/Redis.h"

namespace ruvia {

namespace {

void requirePositiveRedisTtl(std::chrono::seconds ttl, const char* message) {
    if (ttl.count() <= 0) {
        throw std::invalid_argument(message);
    }
}

Task<RedisTtl> redisTtlCommand(detail::RedisCommandExecutor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource,
    bool secondsPrecision) {
    const auto value =
        co_await detail::redisIntegerCommand(std::move(executor), std::move(args), resource);
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
            throw RedisError(
                RedisError::Code::kProtocolError, "redis TTL reply exceeds milliseconds range");
        }
        milliseconds *= kScale;
    }
    co_return detail::RedisTypesAccess::ttl(RedisTtlState::kExpiring, Milliseconds(milliseconds));
}

}  // namespace

RedisHandle::RedisHandle(detail::RedisPool& generalPool, detail::RedisPool& blockingPool,
    std::pmr::memory_resource* resource, ::ruvia::operation_scope& operationScope) noexcept
    : pool_(&generalPool),
      blockingPool_(&blockingPool),
      resource_(detail::pmrResourceOrDefault(resource)),
      registration_(operationScope, this, &RedisHandle::expire_capability) {}

RedisHandle::RedisHandle(detail::RedisPool& generalPool, detail::RedisPool& blockingPool,
    std::pmr::memory_resource* resource, ::ruvia::operation_scope& operationScope,
    OperationOptions options) noexcept
    : pool_(&generalPool),
      blockingPool_(&blockingPool),
      resource_(detail::pmrResourceOrDefault(resource)),
      operationOptions_(std::move(options)),
      registration_(operationScope, this, &RedisHandle::expire_capability) {}

RedisHandle::RedisHandle(const RedisHandle& other) noexcept
    : pool_(other.pool_),
      blockingPool_(other.blockingPool_),
      resource_(other.resource_),
      operationOptions_(other.operationOptions_),
      registration_(other.registration_, this) {}

RedisHandle RedisHandle::withOptions(OperationOptions options) const {
    registration_.require_active();
    detail::validateOperationOptions(options);
    RedisHandle configured(*this);
    configured.operationOptions_ =
        detail::mergeOperationOptions(operationOptions_, std::move(options));
    return configured;
}

const WorkerHandle& RedisHandle::worker() const& {
    registration_.require_active();
    return pool_->worker_;
}

detail::RedisCommandExecutor RedisHandle::executor() const {
    return executor(*pool_);
}

detail::RedisCommandExecutor RedisHandle::executor(detail::RedisPool& pool) const {
    return detail::RedisCommandExecutor{&pool, operationOptions_};
}

void RedisHandle::expire_capability(void* target) noexcept {
    auto& handle = *static_cast<RedisHandle*>(target);
    handle.pool_ = nullptr;
    handle.blockingPool_ = nullptr;
    handle.resource_ = nullptr;
    handle.operationOptions_ = {};
}

ScopedOperation<RedisValue> RedisHandle::command(std::span<const std::string_view> args) const {
    return scoped(commandOwned(args));
}

Task<RedisValue> RedisHandle::commandOwned(std::span<const std::string_view> args) const {
    registration_.require_active();
    const bool blocking = detail::validateRedisPooledCommand(args, true);
    auto& selectedPool = blocking ? *blockingPool_ : *pool_;
    if (blocking && !operationOptions_.stopToken.stoppable() &&
        !operationOptions_.timeout.has_value()) {
        throw std::invalid_argument(
            "raw blocking redis command requires a StopToken or finite operation timeout");
    }
    return detail::executeOwnedRedisCommand(
        selectedPool, detail::ownRedisArgs(args, resource_), operationOptions_, resource_);
}

ScopedOperation<void> RedisHandle::ping() const {
    registration_.require_active();
    return scoped(
        detail::executeRedisPing(executor(), detail::ownRedisArgs({"PING"}, resource_), resource_));
}

ScopedOperation<std::pmr::string> RedisHandle::ping(std::string_view message) const {
    registration_.require_active();
    return scoped(detail::redisStatusCommand(
        executor(), detail::ownRedisArgs({"PING", message}, resource_), resource_));
}

ScopedOperation<std::optional<std::pmr::string>> RedisHandle::get(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redisStringCommand(
        executor(), detail::ownRedisArgs({"GET", key}, resource_), resource_));
}

ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> RedisHandle::mget(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return scoped(detail::redisOptionalStringArrayCommand(
        executor(), detail::redisCommandWithKeys("MGET", keys, resource_), resource_));
}

ScopedOperation<RedisSetResult> RedisHandle::set(
    std::string_view key, std::string_view value, RedisSetOptions options) const {
    registration_.require_active();
    auto args = detail::redisSetArgs(key, value, options, resource_);
    return scoped(
        detail::executeRedisSet(executor(), std::move(args), std::move(options), resource_));
}

ScopedOperation<void> RedisHandle::mset(
    std::span<const std::pair<std::string_view, std::string_view>> items) const {
    registration_.require_active();
    return scoped(
        detail::redisOkCommand(executor(), detail::redisMsetArgs(items, resource_), resource_));
}

ScopedOperation<std::optional<std::pmr::string>> RedisHandle::getDel(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redisStringCommand(
        executor(), detail::ownRedisArgs({"GETDEL", key}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::append(
    std::string_view key, std::string_view value) const {
    registration_.require_active();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"APPEND", key, value}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::strlen(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"STRLEN", key}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::incrBy(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    auto amount = detail::redisIntString(value, resource_);
    return scoped(detail::redisIntegerCommand(executor(),
        detail::ownRedisArgs({"INCRBY", key, std::string_view(amount)}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::decr(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"DECR", key}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::decrBy(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    auto amount = detail::redisIntString(value, resource_);
    return scoped(detail::redisIntegerCommand(executor(),
        detail::ownRedisArgs({"DECRBY", key, std::string_view(amount)}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::del(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"DEL", key}, resource_), resource_));
}

ScopedOperation<std::int64_t> RedisHandle::unlink(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"UNLINK", key}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::exists(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::executeRedisIntegerBool(
        executor(), detail::ownRedisArgs({"EXISTS", key}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::touch(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::executeRedisIntegerBool(
        executor(), detail::ownRedisArgs({"TOUCH", key}, resource_), resource_));
}

ScopedOperation<std::pmr::string> RedisHandle::type(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redisStatusCommand(
        executor(), detail::ownRedisArgs({"TYPE", key}, resource_), resource_));
}

ScopedOperation<void> RedisHandle::rename(std::string_view key, std::string_view newKey) const {
    registration_.require_active();
    return scoped(detail::redisOkCommand(
        executor(), detail::ownRedisArgs({"RENAME", key, newKey}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::renameNx(std::string_view key, std::string_view newKey) const {
    registration_.require_active();
    return scoped(detail::executeRedisIntegerBool(
        executor(), detail::ownRedisArgs({"RENAMENX", key, newKey}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::expire(std::string_view key, std::chrono::seconds ttl) const {
    registration_.require_active();
    requirePositiveRedisTtl(ttl, "redis expire TTL must be greater than zero");
    auto ttlValue = detail::redisSecondsString(ttl, resource_);
    return scoped(detail::executeRedisIntegerBool(executor(),
        detail::ownRedisArgs({"EXPIRE", key, std::string_view(ttlValue)}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::expireAt(
    std::string_view key, std::chrono::system_clock::time_point expiresAt) const {
    registration_.require_active();
    auto value = detail::redisMillisecondsString(
        std::chrono::ceil<std::chrono::milliseconds>(expiresAt.time_since_epoch()), resource_);
    return scoped(detail::executeRedisIntegerBool(executor(),
        detail::ownRedisArgs({"PEXPIREAT", key, std::string_view(value)}, resource_), resource_));
}

ScopedOperation<bool> RedisHandle::persist(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::executeRedisIntegerBool(
        executor(), detail::ownRedisArgs({"PERSIST", key}, resource_), resource_));
}

ScopedOperation<RedisTtl> RedisHandle::ttl(std::string_view key) const {
    registration_.require_active();
    return scoped(redisTtlCommand(
        executor(), detail::ownRedisArgs({"TTL", key}, resource_), resource_, true));
}

ScopedOperation<RedisTtl> RedisHandle::pttl(std::string_view key) const {
    registration_.require_active();
    return scoped(redisTtlCommand(
        executor(), detail::ownRedisArgs({"PTTL", key}, resource_), resource_, false));
}

ScopedOperation<std::int64_t> RedisHandle::incr(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redisIntegerCommand(
        executor(), detail::ownRedisArgs({"INCR", key}, resource_), resource_));
}

}  // namespace ruvia
