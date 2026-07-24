#pragma once

#include "ruvia/web/redis/RedisTransaction.h"
#include "ruvia/web/ScopedOperation.h"

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ruvia {

class RedisHandle final : private detail::ScopedCapabilityNode {
public:
    RedisHandle(const RedisHandle& other) noexcept;
    RedisHandle& operator=(const RedisHandle&) = delete;

    ScopedOperation<RedisValue> command(std::span<const std::string_view> args) const;
    ScopedOperation<RedisValue> command(std::initializer_list<std::string_view> args) const = delete;

    ScopedOperation<void> ping() const;
    ScopedOperation<std::pmr::string> ping(std::string_view message) const;
    ScopedOperation<std::optional<std::pmr::string>> get(std::string_view key) const;
    ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> mget(std::span<const std::string_view> keys) const;
    ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> mget(std::initializer_list<std::string_view> keys) const = delete;
    ScopedOperation<void> set(std::string_view key, std::string_view value) const;
    ScopedOperation<std::optional<std::pmr::string>> set(std::string_view key, std::string_view value, RedisSetOptions options) const;
    ScopedOperation<void> mset(std::span<const std::pair<std::string_view, std::string_view>> items) const;
    ScopedOperation<void> mset(std::initializer_list<std::pair<std::string_view, std::string_view>> items) const = delete;
    ScopedOperation<void> setEx(std::string_view key, std::chrono::seconds ttl, std::string_view value) const;
    ScopedOperation<bool> setNx(std::string_view key, std::string_view value) const;
    ScopedOperation<std::optional<std::pmr::string>> getDel(std::string_view key) const;
    ScopedOperation<std::optional<std::pmr::string>> getSet(std::string_view key, std::string_view value) const;
    ScopedOperation<std::int64_t> append(std::string_view key, std::string_view value) const;
    ScopedOperation<std::int64_t> strlen(std::string_view key) const;
    ScopedOperation<std::int64_t> incrBy(std::string_view key, std::int64_t value) const;
    ScopedOperation<std::int64_t> decr(std::string_view key) const;
    ScopedOperation<std::int64_t> decrBy(std::string_view key, std::int64_t value) const;
    ScopedOperation<std::int64_t> del(std::string_view key) const;
    ScopedOperation<std::int64_t> unlink(std::string_view key) const;
    ScopedOperation<bool> exists(std::string_view key) const;
    ScopedOperation<bool> touch(std::string_view key) const;
    ScopedOperation<std::pmr::string> type(std::string_view key) const;
    ScopedOperation<void> rename(std::string_view key, std::string_view newKey) const;
    ScopedOperation<bool> renameNx(std::string_view key, std::string_view newKey) const;
    ScopedOperation<bool> expire(std::string_view key, std::chrono::seconds ttl) const;
    ScopedOperation<bool> expireAt(std::string_view key, std::chrono::seconds unixTime) const;
    ScopedOperation<bool> persist(std::string_view key) const;
    ScopedOperation<std::int64_t> ttl(std::string_view key) const;
    ScopedOperation<std::int64_t> pttl(std::string_view key) const;
    ScopedOperation<std::int64_t> incr(std::string_view key) const;
    ScopedOperation<std::optional<std::pmr::string>> hget(std::string_view key, std::string_view field) const;
    ScopedOperation<std::int64_t> hset(std::string_view key, std::string_view field, std::string_view value) const;
    ScopedOperation<std::int64_t> hset(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fields) const;
    ScopedOperation<std::int64_t> hset(std::string_view key, std::initializer_list<std::pair<std::string_view, std::string_view>> fields) const = delete;
    ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(std::string_view key, std::span<const std::string_view> fields) const;
    ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(std::string_view key, std::initializer_list<std::string_view> fields) const = delete;
    ScopedOperation<std::pmr::vector<RedisKeyValue>> hgetAll(std::string_view key) const;
    ScopedOperation<std::int64_t> hdel(std::string_view key, std::string_view field) const;
    ScopedOperation<bool> hexists(std::string_view key, std::string_view field) const;
    ScopedOperation<std::int64_t> hlen(std::string_view key) const;
    ScopedOperation<std::pmr::vector<std::pmr::string>> hkeys(std::string_view key) const;
    ScopedOperation<std::pmr::vector<std::pmr::string>> hvals(std::string_view key) const;
    ScopedOperation<std::int64_t> hincrBy(std::string_view key, std::string_view field, std::int64_t value) const;
    ScopedOperation<std::int64_t> lpush(std::string_view key, std::string_view value) const;
    ScopedOperation<std::int64_t> rpush(std::string_view key, std::string_view value) const;
    ScopedOperation<std::optional<std::pmr::string>> lpop(std::string_view key) const;
    ScopedOperation<std::optional<std::pmr::string>> rpop(std::string_view key) const;
    ScopedOperation<std::int64_t> llen(std::string_view key) const;
    ScopedOperation<std::pmr::vector<std::pmr::string>> lrange(std::string_view key, std::int64_t start, std::int64_t stop) const;
    ScopedOperation<std::optional<std::pmr::string>> lindex(std::string_view key, std::int64_t index) const;
    ScopedOperation<void> lset(std::string_view key, std::int64_t index, std::string_view value) const;
    ScopedOperation<void> ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const;
    ScopedOperation<std::int64_t> lrem(std::string_view key, std::int64_t count, std::string_view value) const;
    ScopedOperation<std::int64_t> sadd(std::string_view key, std::string_view member) const;
    ScopedOperation<std::int64_t> srem(std::string_view key, std::string_view member) const;
    ScopedOperation<std::pmr::vector<std::pmr::string>> smembers(std::string_view key) const;
    ScopedOperation<std::int64_t> scard(std::string_view key) const;
    ScopedOperation<bool> sismember(std::string_view key, std::string_view member) const;
    ScopedOperation<std::optional<std::pmr::string>> spop(std::string_view key) const;
    ScopedOperation<std::optional<std::pmr::string>> srandMember(std::string_view key) const;
    ScopedOperation<std::pmr::vector<std::pmr::string>> sinter(std::span<const std::string_view> keys) const;
    ScopedOperation<std::pmr::vector<std::pmr::string>> sinter(std::initializer_list<std::string_view> keys) const = delete;
    ScopedOperation<std::pmr::vector<std::pmr::string>> sunion(std::span<const std::string_view> keys) const;
    ScopedOperation<std::pmr::vector<std::pmr::string>> sunion(std::initializer_list<std::string_view> keys) const = delete;
    ScopedOperation<std::pmr::vector<std::pmr::string>> sdiff(std::span<const std::string_view> keys) const;
    ScopedOperation<std::pmr::vector<std::pmr::string>> sdiff(std::initializer_list<std::string_view> keys) const = delete;
    ScopedOperation<std::int64_t> zadd(std::string_view key, double score, std::string_view member) const;
    ScopedOperation<std::int64_t> zrem(std::string_view key, std::string_view member) const;
    ScopedOperation<std::pmr::vector<std::pmr::string>> zrange(std::string_view key, std::int64_t start, std::int64_t stop) const;
    ScopedOperation<std::pmr::vector<RedisScoredValue>> zrangeWithScores(std::string_view key, std::int64_t start, std::int64_t stop) const;
    ScopedOperation<std::optional<double>> zscore(std::string_view key, std::string_view member) const;
    ScopedOperation<std::int64_t> zcard(std::string_view key) const;
    ScopedOperation<std::int64_t> zcount(std::string_view key, double min, double max) const;
    ScopedOperation<RedisScanResult> scan(RedisScanOptions options = {}) const;
    ScopedOperation<RedisHashScanResult> hscan(std::string_view key, RedisScanOptions options = {}) const;
    ScopedOperation<RedisScanResult> sscan(std::string_view key, RedisScanOptions options = {}) const;
    ScopedOperation<RedisZScanResult> zscan(std::string_view key, RedisScanOptions options = {}) const;
    ScopedOperation<RedisValue> eval(std::string_view script, std::span<const std::string_view> keys = {}, std::span<const std::string_view> args = {}) const;
    ScopedOperation<RedisValue> evalSha(std::string_view sha1, std::span<const std::string_view> keys = {}, std::span<const std::string_view> args = {}) const;
    ScopedOperation<std::pmr::string> scriptLoad(std::string_view script) const;
    ScopedOperation<std::pmr::vector<bool>> scriptExists(std::span<const std::string_view> sha1s) const;
    ScopedOperation<std::pmr::vector<bool>> scriptExists(std::initializer_list<std::string_view> sha1s) const = delete;
    ScopedOperation<std::optional<RedisKeyValue>> blpop(std::span<const std::string_view> keys, std::chrono::seconds timeout) const;
    ScopedOperation<std::optional<RedisKeyValue>> brpop(std::span<const std::string_view> keys, std::chrono::seconds timeout) const;

    [[nodiscard]] RedisPipeline pipeline() const;
    [[nodiscard]] RedisTransaction transaction() const;

private:
    friend class detail::RedisRegistry;

    RedisHandle(detail::RedisPool& pool, std::pmr::memory_resource* resource, detail::ScopedOperationScope& operationScope) noexcept;

    template <typename T>
    [[nodiscard]] ScopedOperation<T> scoped(ruvia::Task<T> task) const {
        return detail::makeScopedOperation(operationScope(), std::move(task));
    }

    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;
    detail::RedisPool* pool_;
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia
