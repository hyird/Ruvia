#pragma once

#include "ruvia/web/redis/RedisTransaction.h"
#include "ruvia/web/ScopedOperation.h"
#include "ruvia/web/detail/redis/RedisArgumentPack.h"

#include <array>
#include <chrono>
#include <cstddef>
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
    ScopedOperation<bool> expireAt(std::string_view key, std::chrono::system_clock::time_point expiresAt) const;
    ScopedOperation<bool> persist(std::string_view key) const;
    ScopedOperation<RedisTtl> ttl(std::string_view key) const;
    ScopedOperation<RedisTtl> pttl(std::string_view key) const;
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

    // Multi-argument commands as ordinary arguments: mget(a, b, c) instead of a
    // hand-built array plus a span. Every span overload above clones its
    // arguments into owned storage before returning -- the command is built
    // synchronously and only the owned copy is moved into the coroutine -- so the
    // temporary array each of these creates only has to outlive the call.
    //
    // A span argument is not convertible to string_view and therefore never
    // selects one of these; a caller that already holds a sequence keeps using
    // the span overload unchanged.
    template <typename... Args>
        requires detail::RedisArgumentPack<Args...>
    [[nodiscard]] ScopedOperation<RedisValue> command(Args&&... args) const {
        const std::string_view views[]{std::string_view(args)...};
        return command(std::span<const std::string_view>(views));
    }

    template <typename... Keys>
        requires detail::RedisArgumentPack<Keys...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> mget(Keys&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return mget(std::span<const std::string_view>(views));
    }

    // Alternating key/value arguments: mset(k1, v1, k2, v2).
    template <typename... Args>
        requires detail::RedisPairArgumentPack<Args...>
    [[nodiscard]] ScopedOperation<void> mset(Args&&... args) const {
        const auto items = pairArguments(std::string_view(args)...);
        return mset(std::span<const std::pair<std::string_view, std::string_view>>(items));
    }

    // Four or more arguments; hset(key, field, value) stays on the single-field
    // overload above rather than routing one pair through the batch path.
    template <typename... Args>
        requires(detail::RedisPairArgumentPack<Args...> && sizeof...(Args) >= 4)
    [[nodiscard]] ScopedOperation<std::int64_t> hset(std::string_view key, Args&&... args) const {
        const auto fields = pairArguments(std::string_view(args)...);
        return hset(key, std::span<const std::pair<std::string_view, std::string_view>>(fields));
    }

    template <typename... Fields>
        requires detail::RedisArgumentPack<Fields...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(std::string_view key, Fields&&... fields) const {
        const std::string_view views[]{std::string_view(fields)...};
        return hmget(key, std::span<const std::string_view>(views));
    }

    template <typename... Keys>
        requires detail::RedisArgumentPack<Keys...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::pmr::string>> sinter(Keys&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return sinter(std::span<const std::string_view>(views));
    }

    template <typename... Keys>
        requires detail::RedisArgumentPack<Keys...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::pmr::string>> sunion(Keys&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return sunion(std::span<const std::string_view>(views));
    }

    template <typename... Keys>
        requires detail::RedisArgumentPack<Keys...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<std::pmr::string>> sdiff(Keys&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return sdiff(std::span<const std::string_view>(views));
    }

    template <typename... Sha1s>
        requires detail::RedisArgumentPack<Sha1s...>
    [[nodiscard]] ScopedOperation<std::pmr::vector<bool>> scriptExists(Sha1s&&... sha1s) const {
        const std::string_view views[]{std::string_view(sha1s)...};
        return scriptExists(std::span<const std::string_view>(views));
    }

    [[nodiscard]] RedisPipeline pipeline() const;
    [[nodiscard]] RedisTransaction transaction() const;

private:
    // Reassembles a flat alternating argument list into the pair sequence the
    // span overloads take.
    template <typename... Args>
    [[nodiscard]] static constexpr auto pairArguments(Args... args) {
        static_assert(sizeof...(Args) % 2 == 0);
        const std::string_view flat[]{args...};
        std::array<std::pair<std::string_view, std::string_view>, sizeof...(Args) / 2> pairs{};
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            pairs[index] = {flat[2 * index], flat[2 * index + 1]};
        }
        return pairs;
    }

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
