#include "ruvia/redis/Redis.h"

#include "RedisHandleHelpers.h"
#include "../RedisInternal.h"
#include "RedisUtils.h"

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

[[nodiscard]] std::pmr::vector<std::pmr::string> redisScanArgs(
    std::string_view command,
    const RedisScanOptions& options,
    std::pmr::memory_resource* resource) {
    auto cursor = detail::redisCursorString(options.cursor, resource);
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(6);
    detail::emplaceRedisString(args, command);
    args.emplace_back(std::move(cursor));
    detail::appendRedisScanOptions(args, options, resource);
    return args;
}

[[nodiscard]] std::pmr::vector<std::pmr::string> redisKeyScanArgs(
    std::string_view command,
    std::string_view key,
    const RedisScanOptions& options,
    std::pmr::memory_resource* resource) {
    auto cursor = detail::redisCursorString(options.cursor, resource);
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(7);
    detail::emplaceRedisString(args, command);
    detail::emplaceRedisString(args, key);
    args.emplace_back(std::move(cursor));
    detail::appendRedisScanOptions(args, options, resource);
    return args;
}

Task<RedisScanResult> executeRedisScan(
    detail::RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await detail::executeOwnedRedisCommand(pool, std::move(args), resource);
    co_return detail::parseRedisScanResult(value, resource);
}

Task<RedisHashScanResult> executeRedisHashScan(
    detail::RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await detail::executeOwnedRedisCommand(pool, std::move(args), resource);
    co_return detail::parseRedisHashScanResult(value, resource);
}

Task<RedisZScanResult> executeRedisZScan(
    detail::RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await detail::executeOwnedRedisCommand(pool, std::move(args), resource);
    co_return detail::parseRedisZScanResult(value, resource);
}

[[nodiscard]] std::chrono::milliseconds redisBlockingPopClientTimeout(std::chrono::seconds timeout) noexcept {
    if (timeout <= std::chrono::seconds(0)) {
        return std::chrono::milliseconds(0);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(timeout) + std::chrono::seconds(1);
}

Task<std::optional<RedisKeyValue>> executeRedisBlockingPop(
    detail::RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::chrono::seconds timeout,
    std::pmr::memory_resource* resource) {
    auto reply = co_await pool.executeWithTimeout(
        std::span<const std::pmr::string>(args.data(), args.size()),
        redisBlockingPopClientTimeout(timeout),
        resource);
    co_return detail::parseRedisBlockingPopReply(reply, resource);
}

}  // namespace

Task<RedisScanResult> RedisHandle::scan(RedisScanOptions options) const {
    return executeRedisScan(pool_, redisScanArgs("SCAN", options, resource_), resource_);
}

Task<RedisHashScanResult> RedisHandle::hscan(std::string_view key, RedisScanOptions options) const {
    return executeRedisHashScan(pool_, redisKeyScanArgs("HSCAN", key, options, resource_), resource_);
}

Task<RedisScanResult> RedisHandle::sscan(std::string_view key, RedisScanOptions options) const {
    return executeRedisScan(pool_, redisKeyScanArgs("SSCAN", key, options, resource_), resource_);
}

Task<RedisZScanResult> RedisHandle::zscan(std::string_view key, RedisScanOptions options) const {
    return executeRedisZScan(pool_, redisKeyScanArgs("ZSCAN", key, options, resource_), resource_);
}

Task<RedisValue> RedisHandle::eval(
    std::string_view script,
    std::span<const std::string_view> keys,
    std::span<const std::string_view> args) const {
    return detail::executeOwnedRedisCommand(pool_, detail::redisEvalArgs("EVAL", script, keys, args, resource_), resource_);
}

Task<RedisValue> RedisHandle::evalSha(
    std::string_view sha1,
    std::span<const std::string_view> keys,
    std::span<const std::string_view> args) const {
    return detail::executeOwnedRedisCommand(pool_, detail::redisEvalArgs("EVALSHA", sha1, keys, args, resource_), resource_);
}

Task<std::pmr::string> RedisHandle::scriptLoad(std::string_view script) const {
    return detail::redisStatusCommand(pool_, detail::ownRedisArgs({"SCRIPT", "LOAD", script}, resource_), resource_);
}

Task<std::pmr::vector<bool>> RedisHandle::scriptExists(std::span<const std::string_view> sha1s) const {
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
    return detail::redisBoolArrayCommand(pool_, std::move(args), resource_);
}

Task<std::optional<RedisKeyValue>> RedisHandle::blpop(
    std::span<const std::string_view> keys,
    std::chrono::seconds timeout) const {
    return executeRedisBlockingPop(
        pool_,
        detail::redisBlockingPopArgs("BLPOP", keys, timeout, resource_),
        timeout,
        resource_);
}

Task<std::optional<RedisKeyValue>> RedisHandle::brpop(
    std::span<const std::string_view> keys,
    std::chrono::seconds timeout) const {
    return executeRedisBlockingPop(
        pool_,
        detail::redisBlockingPopArgs("BRPOP", keys, timeout, resource_),
        timeout,
        resource_);
}

RedisPipeline RedisHandle::pipeline() const {
    return RedisPipeline(pool_, resource_);
}

RedisTransaction RedisHandle::transaction() const {
    return RedisTransaction(pipeline());
}

}  // namespace ruvia
