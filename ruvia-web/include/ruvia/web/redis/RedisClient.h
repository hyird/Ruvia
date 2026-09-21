#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/EventLoop.h"
#include "ruvia/web/redis/RedisHandle.h"
#include "ruvia/web/redis/RedisRepository.h"

namespace ruvia {
namespace detail {
class RedisClientState;
}

// One Redis client bound to one EventLoop, independent of HTTP and App.
// Handles, repositories, cold operations and PMR results borrow this owner.
// Destroy them on the bound loop before destroying the client; shutdown joins
// started operations but does not extend the allocator lifetime of results.
class RedisClient final {
public:
    RedisClient(EventLoop loop, const RedisConfig& config);
    ~RedisClient();
    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;
    RedisClient(RedisClient&&) = delete;
    RedisClient& operator=(RedisClient&&) = delete;

    // Lazy, single-start and worker-affine, like ordinary commands.
    [[nodiscard]] Task<void> connect() &;
    Task<void> connect() && = delete;
    [[nodiscard]] RedisHandle withOptions(OperationOptions options) const&;
    RedisHandle withOptions(OperationOptions) const&& = delete;

    template <typename Entity>
    [[nodiscard]] RedisRepository<Entity> getRepository(
        const RedisRepositoryConfig& config = {}) const& {
        return withOptions({}).template getRepository<Entity>(config);
    }
    template <typename Entity>
    RedisRepository<Entity> getRepository(const RedisRepositoryConfig& = {}) const&& = delete;

    [[nodiscard]] RedisPipeline pipeline() const& {
        return withOptions({}).pipeline();
    }
    RedisPipeline pipeline() const&& = delete;
    [[nodiscard]] RedisTransaction transaction() const& {
        return withOptions({}).transaction();
    }
    RedisTransaction transaction() const&& = delete;

    ScopedOperation<RedisValue> command(std::span<const std::string_view> args) const& {
        return withOptions({}).command(args);
    }

    ScopedOperation<RedisValue> command(std::span<const std::string_view> args) const&& = delete;

    ScopedOperation<RedisValue> command(
        std::initializer_list<std::string_view> args) const& = delete;

    ScopedOperation<RedisValue> command(
        std::initializer_list<std::string_view> args) const&& = delete;

    ScopedOperation<void> ping() const& {
        return withOptions({}).ping();
    }

    ScopedOperation<void> ping() const&& = delete;

    ScopedOperation<std::pmr::string> ping(std::string_view message) const& {
        return withOptions({}).ping(message);
    }

    ScopedOperation<std::pmr::string> ping(std::string_view message) const&& = delete;

    ScopedOperation<std::optional<std::pmr::string>> get(std::string_view key) const& {
        return withOptions({}).get(key);
    }

    ScopedOperation<std::optional<std::pmr::string>> get(std::string_view key) const&& = delete;

    ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        std::span<const std::string_view> keys) const& {
        return withOptions({}).mget(keys);
    }

    ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        std::span<const std::string_view> keys) const&& = delete;

    ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        std::initializer_list<std::string_view> keys) const& = delete;

    ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        std::initializer_list<std::string_view> keys) const&& = delete;

    ScopedOperation<RedisSetResult> set(
        std::string_view key, std::string_view value, RedisSetOptions options = {}) const& {
        return withOptions({}).set(key, value, options);
    }

    ScopedOperation<RedisSetResult> set(
        std::string_view key, std::string_view value, RedisSetOptions options = {}) const&& = delete;

    ScopedOperation<void> mset(
        std::span<const std::pair<std::string_view, std::string_view>> items) const& {
        return withOptions({}).mset(items);
    }

    ScopedOperation<void> mset(
        std::span<const std::pair<std::string_view, std::string_view>> items) const&& = delete;

    ScopedOperation<void> mset(
        std::initializer_list<std::pair<std::string_view, std::string_view>> items) const& = delete;

    ScopedOperation<void> mset(
        std::initializer_list<std::pair<std::string_view, std::string_view>> items) const&& = delete;

    ScopedOperation<std::optional<std::pmr::string>> getDel(std::string_view key) const& {
        return withOptions({}).getDel(key);
    }

    ScopedOperation<std::optional<std::pmr::string>> getDel(std::string_view key) const&& = delete;

    ScopedOperation<std::int64_t> append(std::string_view key, std::string_view value) const& {
        return withOptions({}).append(key, value);
    }

    ScopedOperation<std::int64_t> append(std::string_view key, std::string_view value) const&& = delete;

    ScopedOperation<std::int64_t> strlen(std::string_view key) const& {
        return withOptions({}).strlen(key);
    }

    ScopedOperation<std::int64_t> strlen(std::string_view key) const&& = delete;

    ScopedOperation<std::int64_t> incrBy(std::string_view key, std::int64_t value) const& {
        return withOptions({}).incrBy(key, value);
    }

    ScopedOperation<std::int64_t> incrBy(std::string_view key, std::int64_t value) const&& = delete;

    ScopedOperation<std::int64_t> decr(std::string_view key) const& {
        return withOptions({}).decr(key);
    }

    ScopedOperation<std::int64_t> decr(std::string_view key) const&& = delete;

    ScopedOperation<std::int64_t> decrBy(std::string_view key, std::int64_t value) const& {
        return withOptions({}).decrBy(key, value);
    }

    ScopedOperation<std::int64_t> decrBy(std::string_view key, std::int64_t value) const&& = delete;

    ScopedOperation<std::int64_t> del(std::string_view key) const& {
        return withOptions({}).del(key);
    }

    ScopedOperation<std::int64_t> del(std::string_view key) const&& = delete;

    ScopedOperation<std::int64_t> unlink(std::string_view key) const& {
        return withOptions({}).unlink(key);
    }

    ScopedOperation<std::int64_t> unlink(std::string_view key) const&& = delete;

    ScopedOperation<bool> exists(std::string_view key) const& {
        return withOptions({}).exists(key);
    }

    ScopedOperation<bool> exists(std::string_view key) const&& = delete;

    ScopedOperation<bool> touch(std::string_view key) const& {
        return withOptions({}).touch(key);
    }

    ScopedOperation<bool> touch(std::string_view key) const&& = delete;

    ScopedOperation<std::pmr::string> type(std::string_view key) const& {
        return withOptions({}).type(key);
    }

    ScopedOperation<std::pmr::string> type(std::string_view key) const&& = delete;

    ScopedOperation<void> rename(std::string_view key, std::string_view newKey) const& {
        return withOptions({}).rename(key, newKey);
    }

    ScopedOperation<void> rename(std::string_view key, std::string_view newKey) const&& = delete;

    ScopedOperation<bool> renameNx(std::string_view key, std::string_view newKey) const& {
        return withOptions({}).renameNx(key, newKey);
    }

    ScopedOperation<bool> renameNx(std::string_view key, std::string_view newKey) const&& = delete;

    ScopedOperation<bool> expire(std::string_view key, std::chrono::seconds ttl) const& {
        return withOptions({}).expire(key, ttl);
    }

    ScopedOperation<bool> expire(std::string_view key, std::chrono::seconds ttl) const&& = delete;

    ScopedOperation<bool> expireAt(
        std::string_view key, std::chrono::system_clock::time_point expiresAt) const& {
        return withOptions({}).expireAt(key, expiresAt);
    }

    ScopedOperation<bool> expireAt(
        std::string_view key, std::chrono::system_clock::time_point expiresAt) const&& = delete;

    ScopedOperation<bool> persist(std::string_view key) const& {
        return withOptions({}).persist(key);
    }

    ScopedOperation<bool> persist(std::string_view key) const&& = delete;

    ScopedOperation<RedisTtl> ttl(std::string_view key) const& {
        return withOptions({}).ttl(key);
    }

    ScopedOperation<RedisTtl> ttl(std::string_view key) const&& = delete;

    ScopedOperation<RedisTtl> pttl(std::string_view key) const& {
        return withOptions({}).pttl(key);
    }

    ScopedOperation<RedisTtl> pttl(std::string_view key) const&& = delete;

    ScopedOperation<std::int64_t> incr(std::string_view key) const& {
        return withOptions({}).incr(key);
    }

    ScopedOperation<std::int64_t> incr(std::string_view key) const&& = delete;

    ScopedOperation<std::optional<std::pmr::string>> hget(
        std::string_view key, std::string_view field) const& {
        return withOptions({}).hget(key, field);
    }

    ScopedOperation<std::optional<std::pmr::string>> hget(
        std::string_view key, std::string_view field) const&& = delete;

    ScopedOperation<std::int64_t> hset(
        std::string_view key, std::string_view field, std::string_view value) const& {
        return withOptions({}).hset(key, field, value);
    }

    ScopedOperation<std::int64_t> hset(
        std::string_view key, std::string_view field, std::string_view value) const&& = delete;

    ScopedOperation<std::int64_t> hset(std::string_view key,
        std::span<const std::pair<std::string_view, std::string_view>> fields) const& {
        return withOptions({}).hset(key, fields);
    }

    ScopedOperation<std::int64_t> hset(std::string_view key,
        std::span<const std::pair<std::string_view, std::string_view>> fields) const&& = delete;

    ScopedOperation<std::int64_t> hset(std::string_view key,
        std::initializer_list<std::pair<std::string_view, std::string_view>> fields) const& = delete;

    ScopedOperation<std::int64_t> hset(std::string_view key,
        std::initializer_list<std::pair<std::string_view, std::string_view>> fields) const&& = delete;

    ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, std::span<const std::string_view> fields) const& {
        return withOptions({}).hmget(key, fields);
    }

    ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, std::span<const std::string_view> fields) const&& = delete;

    ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, std::initializer_list<std::string_view> fields) const& = delete;

    ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, std::initializer_list<std::string_view> fields) const&& = delete;

    ScopedOperation<std::pmr::vector<RedisKeyValue>> hgetAll(std::string_view key) const& {
        return withOptions({}).hgetAll(key);
    }

    ScopedOperation<std::pmr::vector<RedisKeyValue>> hgetAll(std::string_view key) const&& = delete;

    ScopedOperation<std::int64_t> hdel(std::string_view key, std::string_view field) const& {
        return withOptions({}).hdel(key, field);
    }

    ScopedOperation<std::int64_t> hdel(std::string_view key, std::string_view field) const&& = delete;

    ScopedOperation<bool> hexists(std::string_view key, std::string_view field) const& {
        return withOptions({}).hexists(key, field);
    }

    ScopedOperation<bool> hexists(std::string_view key, std::string_view field) const&& = delete;

    ScopedOperation<std::int64_t> hlen(std::string_view key) const& {
        return withOptions({}).hlen(key);
    }

    ScopedOperation<std::int64_t> hlen(std::string_view key) const&& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> hkeys(std::string_view key) const& {
        return withOptions({}).hkeys(key);
    }

    ScopedOperation<std::pmr::vector<std::pmr::string>> hkeys(std::string_view key) const&& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> hvals(std::string_view key) const& {
        return withOptions({}).hvals(key);
    }

    ScopedOperation<std::pmr::vector<std::pmr::string>> hvals(std::string_view key) const&& = delete;

    ScopedOperation<std::int64_t> hincrBy(
        std::string_view key, std::string_view field, std::int64_t value) const& {
        return withOptions({}).hincrBy(key, field, value);
    }

    ScopedOperation<std::int64_t> hincrBy(
        std::string_view key, std::string_view field, std::int64_t value) const&& = delete;

    ScopedOperation<std::int64_t> lpush(std::string_view key, std::string_view value) const& {
        return withOptions({}).lpush(key, value);
    }

    ScopedOperation<std::int64_t> lpush(std::string_view key, std::string_view value) const&& = delete;

    ScopedOperation<std::int64_t> rpush(std::string_view key, std::string_view value) const& {
        return withOptions({}).rpush(key, value);
    }

    ScopedOperation<std::int64_t> rpush(std::string_view key, std::string_view value) const&& = delete;

    ScopedOperation<std::optional<std::pmr::string>> lpop(std::string_view key) const& {
        return withOptions({}).lpop(key);
    }

    ScopedOperation<std::optional<std::pmr::string>> lpop(std::string_view key) const&& = delete;

    ScopedOperation<std::optional<std::pmr::string>> rpop(std::string_view key) const& {
        return withOptions({}).rpop(key);
    }

    ScopedOperation<std::optional<std::pmr::string>> rpop(std::string_view key) const&& = delete;

    ScopedOperation<std::int64_t> llen(std::string_view key) const& {
        return withOptions({}).llen(key);
    }

    ScopedOperation<std::int64_t> llen(std::string_view key) const&& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> lrange(
        std::string_view key, std::int64_t start, std::int64_t stop) const& {
        return withOptions({}).lrange(key, start, stop);
    }

    ScopedOperation<std::pmr::vector<std::pmr::string>> lrange(
        std::string_view key, std::int64_t start, std::int64_t stop) const&& = delete;

    ScopedOperation<std::optional<std::pmr::string>> lindex(
        std::string_view key, std::int64_t index) const& {
        return withOptions({}).lindex(key, index);
    }

    ScopedOperation<std::optional<std::pmr::string>> lindex(
        std::string_view key, std::int64_t index) const&& = delete;

    ScopedOperation<void> lset(
        std::string_view key, std::int64_t index, std::string_view value) const& {
        return withOptions({}).lset(key, index, value);
    }

    ScopedOperation<void> lset(
        std::string_view key, std::int64_t index, std::string_view value) const&& = delete;

    ScopedOperation<void> ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const& {
        return withOptions({}).ltrim(key, start, stop);
    }

    ScopedOperation<void> ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const&& = delete;

    ScopedOperation<std::int64_t> lrem(
        std::string_view key, std::int64_t count, std::string_view value) const& {
        return withOptions({}).lrem(key, count, value);
    }

    ScopedOperation<std::int64_t> lrem(
        std::string_view key, std::int64_t count, std::string_view value) const&& = delete;

    ScopedOperation<std::int64_t> sadd(std::string_view key, std::string_view member) const& {
        return withOptions({}).sadd(key, member);
    }

    ScopedOperation<std::int64_t> sadd(std::string_view key, std::string_view member) const&& = delete;

    ScopedOperation<std::int64_t> srem(std::string_view key, std::string_view member) const& {
        return withOptions({}).srem(key, member);
    }

    ScopedOperation<std::int64_t> srem(std::string_view key, std::string_view member) const&& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> smembers(std::string_view key) const& {
        return withOptions({}).smembers(key);
    }

    ScopedOperation<std::pmr::vector<std::pmr::string>> smembers(std::string_view key) const&& = delete;

    ScopedOperation<std::int64_t> scard(std::string_view key) const& {
        return withOptions({}).scard(key);
    }

    ScopedOperation<std::int64_t> scard(std::string_view key) const&& = delete;

    ScopedOperation<bool> sismember(std::string_view key, std::string_view member) const& {
        return withOptions({}).sismember(key, member);
    }

    ScopedOperation<bool> sismember(std::string_view key, std::string_view member) const&& = delete;

    ScopedOperation<std::optional<std::pmr::string>> spop(std::string_view key) const& {
        return withOptions({}).spop(key);
    }

    ScopedOperation<std::optional<std::pmr::string>> spop(std::string_view key) const&& = delete;

    ScopedOperation<std::optional<std::pmr::string>> srandMember(std::string_view key) const& {
        return withOptions({}).srandMember(key);
    }

    ScopedOperation<std::optional<std::pmr::string>> srandMember(std::string_view key) const&& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> sinter(
        std::span<const std::string_view> keys) const& {
        return withOptions({}).sinter(keys);
    }

    ScopedOperation<std::pmr::vector<std::pmr::string>> sinter(
        std::span<const std::string_view> keys) const&& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> sinter(
        std::initializer_list<std::string_view> keys) const& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> sinter(
        std::initializer_list<std::string_view> keys) const&& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> sunion(
        std::span<const std::string_view> keys) const& {
        return withOptions({}).sunion(keys);
    }

    ScopedOperation<std::pmr::vector<std::pmr::string>> sunion(
        std::span<const std::string_view> keys) const&& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> sunion(
        std::initializer_list<std::string_view> keys) const& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> sunion(
        std::initializer_list<std::string_view> keys) const&& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> sdiff(
        std::span<const std::string_view> keys) const& {
        return withOptions({}).sdiff(keys);
    }

    ScopedOperation<std::pmr::vector<std::pmr::string>> sdiff(
        std::span<const std::string_view> keys) const&& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> sdiff(
        std::initializer_list<std::string_view> keys) const& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> sdiff(
        std::initializer_list<std::string_view> keys) const&& = delete;

    ScopedOperation<std::int64_t> zadd(
        std::string_view key, double score, std::string_view member) const& {
        return withOptions({}).zadd(key, score, member);
    }

    ScopedOperation<std::int64_t> zadd(
        std::string_view key, double score, std::string_view member) const&& = delete;

    ScopedOperation<std::int64_t> zrem(std::string_view key, std::string_view member) const& {
        return withOptions({}).zrem(key, member);
    }

    ScopedOperation<std::int64_t> zrem(std::string_view key, std::string_view member) const&& = delete;

    ScopedOperation<std::pmr::vector<std::pmr::string>> zrange(
        std::string_view key, std::int64_t start, std::int64_t stop) const& {
        return withOptions({}).zrange(key, start, stop);
    }

    ScopedOperation<std::pmr::vector<std::pmr::string>> zrange(
        std::string_view key, std::int64_t start, std::int64_t stop) const&& = delete;

    ScopedOperation<std::pmr::vector<RedisScoredValue>> zrangeWithScores(
        std::string_view key, std::int64_t start, std::int64_t stop) const& {
        return withOptions({}).zrangeWithScores(key, start, stop);
    }

    ScopedOperation<std::pmr::vector<RedisScoredValue>> zrangeWithScores(
        std::string_view key, std::int64_t start, std::int64_t stop) const&& = delete;

    ScopedOperation<std::optional<double>> zscore(
        std::string_view key, std::string_view member) const& {
        return withOptions({}).zscore(key, member);
    }

    ScopedOperation<std::optional<double>> zscore(
        std::string_view key, std::string_view member) const&& = delete;

    ScopedOperation<std::int64_t> zcard(std::string_view key) const& {
        return withOptions({}).zcard(key);
    }

    ScopedOperation<std::int64_t> zcard(std::string_view key) const&& = delete;

    ScopedOperation<std::int64_t> zcount(std::string_view key, double min, double max) const& {
        return withOptions({}).zcount(key, min, max);
    }

    ScopedOperation<std::int64_t> zcount(std::string_view key, double min, double max) const&& = delete;

    ScopedOperation<RedisScanResult> scan(RedisScanOptions options = {}) const& {
        return withOptions({}).scan(options);
    }

    ScopedOperation<RedisScanResult> scan(RedisScanOptions options = {}) const&& = delete;

    ScopedOperation<RedisHashScanResult> hscan(
        std::string_view key, RedisScanOptions options = {}) const& {
        return withOptions({}).hscan(key, options);
    }

    ScopedOperation<RedisHashScanResult> hscan(
        std::string_view key, RedisScanOptions options = {}) const&& = delete;

    ScopedOperation<RedisScanResult> sscan(
        std::string_view key, RedisScanOptions options = {}) const& {
        return withOptions({}).sscan(key, options);
    }

    ScopedOperation<RedisScanResult> sscan(
        std::string_view key, RedisScanOptions options = {}) const&& = delete;

    ScopedOperation<RedisZScanResult> zscan(
        std::string_view key, RedisScanOptions options = {}) const& {
        return withOptions({}).zscan(key, options);
    }

    ScopedOperation<RedisZScanResult> zscan(
        std::string_view key, RedisScanOptions options = {}) const&& = delete;

    ScopedOperation<RedisValue> eval(std::string_view script,
        std::span<const std::string_view> keys = {},
        std::span<const std::string_view> args = {}) const& {
        return withOptions({}).eval(script, keys, args);
    }

    ScopedOperation<RedisValue> eval(std::string_view script,
        std::span<const std::string_view> keys = {},
        std::span<const std::string_view> args = {}) const&& = delete;

    ScopedOperation<RedisValue> evalSha(std::string_view sha1,
        std::span<const std::string_view> keys = {},
        std::span<const std::string_view> args = {}) const& {
        return withOptions({}).evalSha(sha1, keys, args);
    }

    ScopedOperation<RedisValue> evalSha(std::string_view sha1,
        std::span<const std::string_view> keys = {},
        std::span<const std::string_view> args = {}) const&& = delete;

    ScopedOperation<std::pmr::string> scriptLoad(std::string_view script) const& {
        return withOptions({}).scriptLoad(script);
    }

    ScopedOperation<std::pmr::string> scriptLoad(std::string_view script) const&& = delete;

    ScopedOperation<std::pmr::vector<bool>> scriptExists(
        std::span<const std::string_view> sha1s) const& {
        return withOptions({}).scriptExists(sha1s);
    }

    ScopedOperation<std::pmr::vector<bool>> scriptExists(
        std::span<const std::string_view> sha1s) const&& = delete;

    ScopedOperation<std::pmr::vector<bool>> scriptExists(
        std::initializer_list<std::string_view> sha1s) const& = delete;

    ScopedOperation<std::pmr::vector<bool>> scriptExists(
        std::initializer_list<std::string_view> sha1s) const&& = delete;

    ScopedOperation<std::optional<RedisKeyValue>> blpop(
        std::span<const std::string_view> keys, RedisBlockWait wait) const& {
        return withOptions({}).blpop(keys, wait);
    }

    ScopedOperation<std::optional<RedisKeyValue>> blpop(
        std::span<const std::string_view> keys, RedisBlockWait wait) const&& = delete;

    ScopedOperation<std::optional<RedisKeyValue>> brpop(
        std::span<const std::string_view> keys, RedisBlockWait wait) const& {
        return withOptions({}).brpop(keys, wait);
    }

    ScopedOperation<std::optional<RedisKeyValue>> brpop(
        std::span<const std::string_view> keys, RedisBlockWait wait) const&& = delete;

    ScopedOperation<std::optional<RedisXReadGroupResult>> xreadGroup(std::string_view group,
        std::string_view consumer, std::span<const RedisStreamReadView> streams,
        RedisXReadGroupOptions options = {}) const& {
        return withOptions({}).xreadGroup(group, consumer, streams, options);
    }

    ScopedOperation<std::optional<RedisXReadGroupResult>> xreadGroup(std::string_view group,
        std::string_view consumer, std::span<const RedisStreamReadView> streams,
        RedisXReadGroupOptions options = {}) const&& = delete;

    template <typename... Args>
        requires detail::RedisArgumentPack<Args...>
    [[nodiscard]] ScopedOperation<RedisValue> command(Args&&... args) const& {
        return withOptions({}).command(std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires detail::RedisArgumentPack<Args...>
    [[nodiscard]] ScopedOperation<RedisValue> command(Args&&... args) const&& = delete;

    template <typename... Keys>
        requires detail::RedisArgumentPack<Keys...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        Keys&&... keys) const& {
        return withOptions({}).mget(std::forward<Keys>(keys)...);
    }

    template <typename... Keys>
        requires detail::RedisArgumentPack<Keys...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        Keys&&... keys) const&& = delete;

    template <typename... Args>
        requires detail::RedisPairArgumentPack<Args...>
    [[nodiscard]] ScopedOperation<void> mset(Args&&... args) const& {
        return withOptions({}).mset(std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires detail::RedisPairArgumentPack<Args...>
    [[nodiscard]] ScopedOperation<void> mset(Args&&... args) const&& = delete;

    template <typename... Args>
        requires(detail::RedisPairArgumentPack<Args...> && sizeof...(Args) >= 4)
    [[nodiscard]] ScopedOperation<std::int64_t> hset(std::string_view key, Args&&... args) const& {
        return withOptions({}).hset(key, std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires(detail::RedisPairArgumentPack<Args...> && sizeof...(Args) >= 4)
    [[nodiscard]] ScopedOperation<std::int64_t> hset(std::string_view key, Args&&... args) const&& = delete;

    template <typename... Fields>
        requires detail::RedisArgumentPack<Fields...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, Fields&&... fields) const& {
        return withOptions({}).hmget(key, std::forward<Fields>(fields)...);
    }

    template <typename... Fields>
        requires detail::RedisArgumentPack<Fields...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, Fields&&... fields) const&& = delete;

    template <typename... Keys>
        requires detail::RedisArgumentPack<Keys...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::pmr::string>> sinter(Keys&&... keys) const& {
        return withOptions({}).sinter(std::forward<Keys>(keys)...);
    }

    template <typename... Keys>
        requires detail::RedisArgumentPack<Keys...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::pmr::string>> sinter(Keys&&... keys) const&& = delete;

    template <typename... Keys>
        requires detail::RedisArgumentPack<Keys...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::pmr::string>> sunion(Keys&&... keys) const& {
        return withOptions({}).sunion(std::forward<Keys>(keys)...);
    }

    template <typename... Keys>
        requires detail::RedisArgumentPack<Keys...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::pmr::string>> sunion(Keys&&... keys) const&& = delete;

    template <typename... Keys>
        requires detail::RedisArgumentPack<Keys...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::pmr::string>> sdiff(Keys&&... keys) const& {
        return withOptions({}).sdiff(std::forward<Keys>(keys)...);
    }

    template <typename... Keys>
        requires detail::RedisArgumentPack<Keys...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::pmr::string>> sdiff(Keys&&... keys) const&& = delete;

    template <typename... Sha1s>
        requires detail::RedisArgumentPack<Sha1s...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<bool>> scriptExists(Sha1s&&... sha1s) const& {
        return withOptions({}).scriptExists(std::forward<Sha1s>(sha1s)...);
    }

    template <typename... Sha1s>
        requires detail::RedisArgumentPack<Sha1s...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<bool>> scriptExists(Sha1s&&... sha1s) const&& = delete;

    // Thread-safe close request; sockets are closed only on the bound loop.
    void close() noexcept;
    // Await on the bound loop to cancel and join all started operations.
    [[nodiscard]] Task<void> shutdown() &;
    Task<void> shutdown() && = delete;
    [[nodiscard]] const WorkerHandle& worker() const& noexcept;
    const WorkerHandle& worker() const&& = delete;

private:
    std::shared_ptr<detail::RedisClientState> state_;
};

}  // namespace ruvia
