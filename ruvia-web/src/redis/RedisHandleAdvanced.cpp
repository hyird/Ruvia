#include "ruvia/web/redis/Redis.h"

#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace ruvia {
namespace {

[[nodiscard]] std::pmr::vector<std::pmr::string> redisScanArgs(std::string_view command, const RedisScanOptions& options, std::pmr::memory_resource* resource) {
    auto cursor = detail::redisCursorString(options.cursor, resource);
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(6);
    detail::emplaceRedisString(args, command);
    args.emplace_back(std::move(cursor));
    detail::appendRedisScanOptions(args, options, resource);
    return args;
}

[[nodiscard]] std::pmr::vector<std::pmr::string> redisKeyScanArgs(std::string_view command, std::string_view key, const RedisScanOptions& options, std::pmr::memory_resource* resource) {
    auto cursor = detail::redisCursorString(options.cursor, resource);
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(7);
    detail::emplaceRedisString(args, command);
    detail::emplaceRedisString(args, key);
    args.emplace_back(std::move(cursor));
    detail::appendRedisScanOptions(args, options, resource);
    return args;
}

Task<RedisScanResult> executeRedisScan(detail::RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await detail::executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    co_return detail::parseRedisScanResult(value, resource);
}

Task<RedisHashScanResult> executeRedisHashScan(detail::RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await detail::executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    co_return detail::parseRedisHashScanResult(value, resource);
}

Task<RedisZScanResult> executeRedisZScan(detail::RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await detail::executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    co_return detail::parseRedisZScanResult(value, resource);
}

void constrainOperationTimeout(RedisOperationOptions& options, std::optional<std::chrono::milliseconds> timeout) noexcept {
    if (timeout.has_value() && (!options.timeout.has_value() || *timeout < *options.timeout)) {
        options.timeout = timeout;
    }
}

[[nodiscard]] std::optional<std::chrono::milliseconds> redisBlockClientTimeout(std::chrono::milliseconds timeout) noexcept {
    constexpr auto grace = std::chrono::seconds(1);
    if (timeout > std::chrono::milliseconds::max() - grace) {
        return std::chrono::milliseconds::max();
    }
    return timeout + grace;
}

void requireCancelableInfiniteBlock(const detail::RedisPool& pool, const RedisOperationOptions& options) {
    if (!options.stopToken.stoppable() && !options.timeout.has_value() && !pool.hasCommandTimeout()) {
        throw std::invalid_argument("infinite redis block requires a StopToken or finite command timeout");
    }
}

Task<std::optional<RedisKeyValue>> executeRedisBlockingPop(detail::RedisPool& pool, std::pmr::vector<std::pmr::string> args, std::chrono::seconds timeout, RedisOperationOptions options, std::pmr::memory_resource* resource) {
    constrainOperationTimeout(options, detail::redisBlockingPopClientTimeout(timeout));
    auto reply = co_await detail::executeOwnedRedisCommand(pool, std::move(args), std::move(options), resource);
    co_return detail::parseRedisBlockingPopReply(reply, resource);
}

Task<std::optional<RedisXReadGroupResult>> executeRedisXReadGroup(detail::RedisPool& pool, std::pmr::vector<std::pmr::string> args, RedisOperationOptions options, std::pmr::memory_resource* resource) {
    auto reply = co_await detail::executeOwnedRedisCommand(pool, std::move(args), std::move(options), resource);
    co_return detail::parseRedisXReadGroupReply(reply, resource);
}

}  // namespace

ScopedOperation<RedisScanResult> RedisHandle::scan(RedisScanOptions options) const {
    requireActive();
    return scoped(executeRedisScan(executor(), redisScanArgs("SCAN", options, resource_), resource_));
}

ScopedOperation<RedisHashScanResult> RedisHandle::hscan(std::string_view key, RedisScanOptions options) const {
    requireActive();
    return scoped(executeRedisHashScan(executor(), redisKeyScanArgs("HSCAN", key, options, resource_), resource_));
}

ScopedOperation<RedisScanResult> RedisHandle::sscan(std::string_view key, RedisScanOptions options) const {
    requireActive();
    return scoped(executeRedisScan(executor(), redisKeyScanArgs("SSCAN", key, options, resource_), resource_));
}

ScopedOperation<RedisZScanResult> RedisHandle::zscan(std::string_view key, RedisScanOptions options) const {
    requireActive();
    return scoped(executeRedisZScan(executor(), redisKeyScanArgs("ZSCAN", key, options, resource_), resource_));
}

ScopedOperation<RedisValue> RedisHandle::eval(std::string_view script, std::span<const std::string_view> keys, std::span<const std::string_view> args) const {
    requireActive();
    return scoped(detail::executeOwnedRedisCommand(executor(), detail::redisEvalArgs("EVAL", script, keys, args, resource_), resource_));
}

ScopedOperation<RedisValue> RedisHandle::evalSha(std::string_view sha1, std::span<const std::string_view> keys, std::span<const std::string_view> args) const {
    requireActive();
    return scoped(detail::executeOwnedRedisCommand(executor(), detail::redisEvalArgs("EVALSHA", sha1, keys, args, resource_), resource_));
}

ScopedOperation<std::pmr::string> RedisHandle::scriptLoad(std::string_view script) const {
    requireActive();
    return scoped(detail::redisStatusCommand(executor(), detail::ownRedisArgs({"SCRIPT", "LOAD", script}, resource_), resource_));
}

ScopedOperation<std::pmr::vector<bool>> RedisHandle::scriptExists(std::span<const std::string_view> sha1s) const {
    requireActive();
    if (sha1s.empty()) {
        throw std::invalid_argument("redis script exists requires at least one sha1");
    }
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(sha1s.size() + 2);
    detail::emplaceRedisString(args, "SCRIPT");
    detail::emplaceRedisString(args, "EXISTS");
    for (const auto sha1 : sha1s) {
        detail::emplaceRedisString(args, sha1);
    }
    return scoped(detail::redisBoolArrayCommand(executor(), std::move(args), resource_));
}

ScopedOperation<std::optional<RedisKeyValue>> RedisHandle::blpop(std::span<const std::string_view> keys, std::chrono::seconds timeout, RedisOperationOptions options) const {
    requireActive();
    options = detail::mergeRedisOperationOptions(operationOptions_, std::move(options));
    detail::validateRedisOperationOptions(options);
    if (timeout == std::chrono::seconds::zero()) {
        requireCancelableInfiniteBlock(*blockingPool_, options);
    }
    return scoped(executeRedisBlockingPop(*blockingPool_, detail::redisBlockingPopArgs("BLPOP", keys, timeout, resource_), timeout, std::move(options), resource_));
}

ScopedOperation<std::optional<RedisKeyValue>> RedisHandle::brpop(std::span<const std::string_view> keys, std::chrono::seconds timeout, RedisOperationOptions options) const {
    requireActive();
    options = detail::mergeRedisOperationOptions(operationOptions_, std::move(options));
    detail::validateRedisOperationOptions(options);
    if (timeout == std::chrono::seconds::zero()) {
        requireCancelableInfiniteBlock(*blockingPool_, options);
    }
    return scoped(executeRedisBlockingPop(*blockingPool_, detail::redisBlockingPopArgs("BRPOP", keys, timeout, resource_), timeout, std::move(options), resource_));
}

ScopedOperation<std::optional<RedisXReadGroupResult>> RedisHandle::xreadGroup(std::string_view group, std::string_view consumer, std::span<const RedisStreamReadView> streams, RedisXReadGroupOptions options) const {
    requireActive();
    options.operation = detail::mergeRedisOperationOptions(operationOptions_, std::move(options.operation));
    detail::validateRedisOperationOptions(options.operation);
    auto args = detail::redisXReadGroupArgs(group, consumer, streams, options, resource_);
    auto* selectedPool = pool_;
    if (options.block.has_value()) {
        selectedPool = blockingPool_;
        if (const auto duration = options.block->duration(); duration.has_value()) {
            constrainOperationTimeout(options.operation, redisBlockClientTimeout(*duration));
        } else {
            requireCancelableInfiniteBlock(*selectedPool, options.operation);
        }
    }
    return scoped(executeRedisXReadGroup(*selectedPool, std::move(args), std::move(options.operation), resource_));
}

RedisPipeline RedisHandle::pipeline() const {
    requireActive();
    return RedisPipeline(*pool_, operationOptions_, resource_, operationScope());
}

RedisTransaction RedisHandle::transaction() const {
    return RedisTransaction(pipeline());
}

}  // namespace ruvia
