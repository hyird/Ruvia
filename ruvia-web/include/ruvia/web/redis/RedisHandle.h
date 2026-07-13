#pragma once

#include "ruvia/web/redis/RedisTransaction.h"

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

class RedisHandle final {
public:
    RedisHandle(const RedisHandle&) = default;
    RedisHandle& operator=(const RedisHandle&) = delete;

    Task<RedisValue> command(std::span<const std::string_view> args) const;
    Task<RedisValue> command(std::initializer_list<std::string_view> args) const = delete;

    Task<void> ping() const;
    Task<std::pmr::string> ping(std::string_view message) const;
    Task<std::optional<std::pmr::string>> get(std::string_view key) const;
    Task<std::pmr::vector<std::optional<std::pmr::string>>> mget(std::span<const std::string_view> keys) const;
    Task<std::pmr::vector<std::optional<std::pmr::string>>> mget(std::initializer_list<std::string_view> keys) const = delete;
    Task<void> set(std::string_view key, std::string_view value) const;
    Task<std::optional<std::pmr::string>> set(std::string_view key, std::string_view value, RedisSetOptions options) const;
    Task<void> mset(std::span<const std::pair<std::string_view, std::string_view>> items) const;
    Task<void> mset(std::initializer_list<std::pair<std::string_view, std::string_view>> items) const = delete;
    Task<void> setEx(std::string_view key, std::chrono::seconds ttl, std::string_view value) const;
    Task<bool> setNx(std::string_view key, std::string_view value) const;
    Task<std::optional<std::pmr::string>> getDel(std::string_view key) const;
    Task<std::optional<std::pmr::string>> getSet(std::string_view key, std::string_view value) const;
    Task<std::int64_t> append(std::string_view key, std::string_view value) const;
    Task<std::int64_t> strlen(std::string_view key) const;
    Task<std::int64_t> incrBy(std::string_view key, std::int64_t value) const;
    Task<std::int64_t> decr(std::string_view key) const;
    Task<std::int64_t> decrBy(std::string_view key, std::int64_t value) const;
    Task<std::int64_t> del(std::string_view key) const;
    Task<std::int64_t> unlink(std::string_view key) const;
    Task<bool> exists(std::string_view key) const;
    Task<bool> touch(std::string_view key) const;
    Task<std::pmr::string> type(std::string_view key) const;
    Task<void> rename(std::string_view key, std::string_view newKey) const;
    Task<bool> renameNx(std::string_view key, std::string_view newKey) const;
    Task<bool> expire(std::string_view key, std::chrono::seconds ttl) const;
    Task<bool> expireAt(std::string_view key, std::chrono::seconds unixTime) const;
    Task<bool> persist(std::string_view key) const;
    Task<std::int64_t> ttl(std::string_view key) const;
    Task<std::int64_t> pttl(std::string_view key) const;
    Task<std::int64_t> incr(std::string_view key) const;
    Task<std::optional<std::pmr::string>> hget(std::string_view key, std::string_view field) const;
    Task<std::int64_t> hset(std::string_view key, std::string_view field, std::string_view value) const;
    Task<std::int64_t> hset(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fields) const;
    Task<std::int64_t> hset(std::string_view key, std::initializer_list<std::pair<std::string_view, std::string_view>> fields) const = delete;
    Task<std::pmr::vector<std::optional<std::pmr::string>>> hmget(std::string_view key, std::span<const std::string_view> fields) const;
    Task<std::pmr::vector<std::optional<std::pmr::string>>> hmget(std::string_view key, std::initializer_list<std::string_view> fields) const = delete;
    Task<std::pmr::vector<RedisKeyValue>> hgetAll(std::string_view key) const;
    Task<std::int64_t> hdel(std::string_view key, std::string_view field) const;
    Task<bool> hexists(std::string_view key, std::string_view field) const;
    Task<std::int64_t> hlen(std::string_view key) const;
    Task<std::pmr::vector<std::pmr::string>> hkeys(std::string_view key) const;
    Task<std::pmr::vector<std::pmr::string>> hvals(std::string_view key) const;
    Task<std::int64_t> hincrBy(std::string_view key, std::string_view field, std::int64_t value) const;
    Task<std::int64_t> lpush(std::string_view key, std::string_view value) const;
    Task<std::int64_t> rpush(std::string_view key, std::string_view value) const;
    Task<std::optional<std::pmr::string>> lpop(std::string_view key) const;
    Task<std::optional<std::pmr::string>> rpop(std::string_view key) const;
    Task<std::int64_t> llen(std::string_view key) const;
    Task<std::pmr::vector<std::pmr::string>> lrange(std::string_view key, std::int64_t start, std::int64_t stop) const;
    Task<std::optional<std::pmr::string>> lindex(std::string_view key, std::int64_t index) const;
    Task<void> lset(std::string_view key, std::int64_t index, std::string_view value) const;
    Task<void> ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const;
    Task<std::int64_t> lrem(std::string_view key, std::int64_t count, std::string_view value) const;
    Task<std::int64_t> sadd(std::string_view key, std::string_view member) const;
    Task<std::int64_t> srem(std::string_view key, std::string_view member) const;
    Task<std::pmr::vector<std::pmr::string>> smembers(std::string_view key) const;
    Task<std::int64_t> scard(std::string_view key) const;
    Task<bool> sismember(std::string_view key, std::string_view member) const;
    Task<std::optional<std::pmr::string>> spop(std::string_view key) const;
    Task<std::optional<std::pmr::string>> srandMember(std::string_view key) const;
    Task<std::pmr::vector<std::pmr::string>> sinter(std::span<const std::string_view> keys) const;
    Task<std::pmr::vector<std::pmr::string>> sinter(std::initializer_list<std::string_view> keys) const = delete;
    Task<std::pmr::vector<std::pmr::string>> sunion(std::span<const std::string_view> keys) const;
    Task<std::pmr::vector<std::pmr::string>> sunion(std::initializer_list<std::string_view> keys) const = delete;
    Task<std::pmr::vector<std::pmr::string>> sdiff(std::span<const std::string_view> keys) const;
    Task<std::pmr::vector<std::pmr::string>> sdiff(std::initializer_list<std::string_view> keys) const = delete;
    Task<std::int64_t> zadd(std::string_view key, double score, std::string_view member) const;
    Task<std::int64_t> zrem(std::string_view key, std::string_view member) const;
    Task<std::pmr::vector<std::pmr::string>> zrange(std::string_view key, std::int64_t start, std::int64_t stop) const;
    Task<std::pmr::vector<RedisScoredValue>> zrangeWithScores(std::string_view key, std::int64_t start, std::int64_t stop) const;
    Task<std::optional<double>> zscore(std::string_view key, std::string_view member) const;
    Task<std::int64_t> zcard(std::string_view key) const;
    Task<std::int64_t> zcount(std::string_view key, double min, double max) const;
    Task<RedisScanResult> scan(RedisScanOptions options = {}) const;
    Task<RedisHashScanResult> hscan(std::string_view key, RedisScanOptions options = {}) const;
    Task<RedisScanResult> sscan(std::string_view key, RedisScanOptions options = {}) const;
    Task<RedisZScanResult> zscan(std::string_view key, RedisScanOptions options = {}) const;
    Task<RedisValue> eval(
        std::string_view script,
        std::span<const std::string_view> keys = {},
        std::span<const std::string_view> args = {}) const;
    Task<RedisValue> evalSha(
        std::string_view sha1,
        std::span<const std::string_view> keys = {},
        std::span<const std::string_view> args = {}) const;
    Task<std::pmr::string> scriptLoad(std::string_view script) const;
    Task<std::pmr::vector<bool>> scriptExists(std::span<const std::string_view> sha1s) const;
    Task<std::pmr::vector<bool>> scriptExists(std::initializer_list<std::string_view> sha1s) const = delete;
    Task<std::optional<RedisKeyValue>> blpop(std::span<const std::string_view> keys, std::chrono::seconds timeout) const;
    Task<std::optional<RedisKeyValue>> brpop(std::span<const std::string_view> keys, std::chrono::seconds timeout) const;

    [[nodiscard]] RedisPipeline pipeline() const;
    [[nodiscard]] RedisTransaction transaction() const;

private:
    friend class detail::RedisRegistry;

    RedisHandle(
        detail::RedisPool& pool,
        std::pmr::memory_resource* resource) noexcept;

    detail::RedisPool& pool_;
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia
