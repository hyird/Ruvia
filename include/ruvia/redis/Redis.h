#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/app/Task.h"

namespace ruvia {

class RequestMemory;

struct RedisConfig {
    std::pmr::string host{"127.0.0.1"};
    std::uint16_t port{6379};
    std::pmr::string username;
    std::pmr::string password;
    std::uint32_t database{0};
    std::size_t poolSizePerWorker{4};
    std::chrono::milliseconds connectTimeout{0};
    std::chrono::milliseconds commandTimeout{0};
    std::chrono::milliseconds acquireTimeout{0};
    std::size_t maxReplyBytes{64 * 1024 * 1024};
    std::size_t maxArrayDepth{64};
    bool tcpNoDelay{true};
    bool keepAlive{false};
};

struct RedisSetOptions {
    std::chrono::milliseconds ttl{0};
    bool nx{false};
    bool xx{false};
    bool get{false};
    bool keepTtl{false};
};

struct RedisScanOptions {
    std::uint64_t cursor{0};
    std::string_view match;
    std::uint64_t count{0};
};

struct RedisKeyValue {
    std::pmr::string key;
    std::pmr::string value;
};

struct RedisScoredValue {
    std::pmr::string value;
    double score{0};
};

struct RedisScanResult {
    std::uint64_t cursor{0};
    std::pmr::vector<std::pmr::string> values;
};

struct RedisHashScanResult {
    std::uint64_t cursor{0};
    std::pmr::vector<RedisKeyValue> entries;
};

struct RedisZScanResult {
    std::uint64_t cursor{0};
    std::pmr::vector<RedisScoredValue> entries;
};

namespace detail {

inline constexpr std::string_view kDefaultRedisAlias = "default";

struct RedisDefinition final {
    std::pmr::string alias;
    RedisConfig config;
};

class RedisPool;
class RedisRegistry;

}  // namespace detail

class RedisError : public std::exception {
public:
    enum class Code {
        kNotConfigured,
        kPoolExhausted,
        kConnectFailed,
        kAuthFailed,
        kProtocolError,
        kCommandError,
        kIoError,
        kTimeout,
        kTransactionAborted
    };

    RedisError(Code code, std::string_view message);

    [[nodiscard]] const char* what() const noexcept override;
    [[nodiscard]] Code code() const noexcept;
    [[nodiscard]] std::string_view message() const noexcept;

private:
    Code code_;
    std::pmr::string message_;
};

class RedisValue final {
public:
    enum class Kind {
        kNull,
        kString,
        kInteger,
        kArray,
        kError
    };

    explicit RedisValue(std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    RedisValue(const RedisValue&) = default;
    RedisValue& operator=(const RedisValue&) = default;
    RedisValue(RedisValue&&) noexcept = default;
    RedisValue& operator=(RedisValue&&) noexcept = default;

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] bool null() const noexcept;
    [[nodiscard]] std::string_view string() const;
    [[nodiscard]] std::int64_t integer() const;
    [[nodiscard]] std::span<const RedisValue> array() const;

    [[nodiscard]] static RedisValue nullValue(std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue stringValue(std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue errorValue(std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue integerValue(std::int64_t value, std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue arrayValue(std::pmr::vector<RedisValue> values, std::pmr::memory_resource* resource);

private:
    friend class detail::RedisPool;

    Kind kind_{Kind::kNull};
    std::pmr::string string_;
    std::int64_t integer_{0};
    std::pmr::vector<RedisValue> array_;
};

class RedisPipeline final {
public:
    RedisPipeline(const RedisPipeline&) = delete;
    RedisPipeline& operator=(const RedisPipeline&) = delete;
    RedisPipeline(RedisPipeline&&) noexcept = default;
    RedisPipeline& operator=(RedisPipeline&&) noexcept = default;

    RedisPipeline& command(std::initializer_list<std::string_view> args);
    RedisPipeline& command(std::span<const std::string_view> args);
    RedisPipeline& get(std::string_view key);
    RedisPipeline& set(std::string_view key, std::string_view value);
    RedisPipeline& getDel(std::string_view key);
    RedisPipeline& getSet(std::string_view key, std::string_view value);
    RedisPipeline& append(std::string_view key, std::string_view value);
    RedisPipeline& strlen(std::string_view key);
    RedisPipeline& del(std::string_view key);
    RedisPipeline& unlink(std::string_view key);
    RedisPipeline& exists(std::string_view key);
    RedisPipeline& touch(std::string_view key);
    RedisPipeline& type(std::string_view key);
    RedisPipeline& rename(std::string_view key, std::string_view newKey);
    RedisPipeline& renameNx(std::string_view key, std::string_view newKey);
    RedisPipeline& incr(std::string_view key);
    RedisPipeline& incrBy(std::string_view key, std::int64_t value);
    RedisPipeline& decr(std::string_view key);
    RedisPipeline& decrBy(std::string_view key, std::int64_t value);
    RedisPipeline& hget(std::string_view key, std::string_view field);
    RedisPipeline& hset(std::string_view key, std::string_view field, std::string_view value);
    RedisPipeline& hdel(std::string_view key, std::string_view field);
    RedisPipeline& hexists(std::string_view key, std::string_view field);
    RedisPipeline& hlen(std::string_view key);
    RedisPipeline& hgetAll(std::string_view key);
    RedisPipeline& lpush(std::string_view key, std::string_view value);
    RedisPipeline& rpush(std::string_view key, std::string_view value);
    RedisPipeline& lpop(std::string_view key);
    RedisPipeline& rpop(std::string_view key);
    RedisPipeline& llen(std::string_view key);
    RedisPipeline& lrange(std::string_view key, std::int64_t start, std::int64_t stop);
    RedisPipeline& sadd(std::string_view key, std::string_view member);
    RedisPipeline& srem(std::string_view key, std::string_view member);
    RedisPipeline& smembers(std::string_view key);
    RedisPipeline& scard(std::string_view key);
    RedisPipeline& zadd(std::string_view key, double score, std::string_view member);
    RedisPipeline& zrem(std::string_view key, std::string_view member);
    RedisPipeline& zrange(std::string_view key, std::int64_t start, std::int64_t stop);
    RedisPipeline& zscore(std::string_view key, std::string_view member);
    RedisPipeline& zcard(std::string_view key);

    Task<std::pmr::vector<RedisValue>> exec();

private:
    friend class RedisHandle;
    friend class RedisTransaction;
    friend class detail::RedisPool;

    struct Command final {
        std::pmr::vector<std::pmr::string> args;
    };

    RedisPipeline(
        detail::RedisPool& pool,
        std::pmr::memory_resource* resource,
        RequestMemory* requestMemory) noexcept;

    detail::RedisPool* pool_{nullptr};
    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
    RequestMemory* requestMemory_{nullptr};
    std::pmr::vector<Command> commands_;
};

class RedisTransaction final {
public:
    RedisTransaction(const RedisTransaction&) = delete;
    RedisTransaction& operator=(const RedisTransaction&) = delete;
    RedisTransaction(RedisTransaction&&) noexcept = default;
    RedisTransaction& operator=(RedisTransaction&&) noexcept = default;

    RedisTransaction& command(std::initializer_list<std::string_view> args);
    RedisTransaction& command(std::span<const std::string_view> args);
    RedisTransaction& watch(std::string_view key);
    RedisTransaction& watch(std::span<const std::string_view> keys);
    RedisTransaction& unwatch();
    RedisTransaction& discard() noexcept;
    RedisTransaction& get(std::string_view key);
    RedisTransaction& set(std::string_view key, std::string_view value);
    RedisTransaction& getDel(std::string_view key);
    RedisTransaction& getSet(std::string_view key, std::string_view value);
    RedisTransaction& append(std::string_view key, std::string_view value);
    RedisTransaction& strlen(std::string_view key);
    RedisTransaction& del(std::string_view key);
    RedisTransaction& unlink(std::string_view key);
    RedisTransaction& exists(std::string_view key);
    RedisTransaction& touch(std::string_view key);
    RedisTransaction& type(std::string_view key);
    RedisTransaction& rename(std::string_view key, std::string_view newKey);
    RedisTransaction& renameNx(std::string_view key, std::string_view newKey);
    RedisTransaction& incr(std::string_view key);
    RedisTransaction& incrBy(std::string_view key, std::int64_t value);
    RedisTransaction& decr(std::string_view key);
    RedisTransaction& decrBy(std::string_view key, std::int64_t value);
    RedisTransaction& hget(std::string_view key, std::string_view field);
    RedisTransaction& hset(std::string_view key, std::string_view field, std::string_view value);
    RedisTransaction& hdel(std::string_view key, std::string_view field);
    RedisTransaction& hexists(std::string_view key, std::string_view field);
    RedisTransaction& hlen(std::string_view key);
    RedisTransaction& hgetAll(std::string_view key);
    RedisTransaction& lpush(std::string_view key, std::string_view value);
    RedisTransaction& rpush(std::string_view key, std::string_view value);
    RedisTransaction& lpop(std::string_view key);
    RedisTransaction& rpop(std::string_view key);
    RedisTransaction& llen(std::string_view key);
    RedisTransaction& lrange(std::string_view key, std::int64_t start, std::int64_t stop);
    RedisTransaction& sadd(std::string_view key, std::string_view member);
    RedisTransaction& srem(std::string_view key, std::string_view member);
    RedisTransaction& smembers(std::string_view key);
    RedisTransaction& scard(std::string_view key);
    RedisTransaction& zadd(std::string_view key, double score, std::string_view member);
    RedisTransaction& zrem(std::string_view key, std::string_view member);
    RedisTransaction& zrange(std::string_view key, std::int64_t start, std::int64_t stop);
    RedisTransaction& zscore(std::string_view key, std::string_view member);
    RedisTransaction& zcard(std::string_view key);

    Task<std::pmr::vector<RedisValue>> exec();

private:
    friend class RedisHandle;

    explicit RedisTransaction(RedisPipeline pipeline) noexcept;

    RedisPipeline pipeline_;
    std::pmr::vector<RedisPipeline::Command> watches_;
    bool discarded_{false};
};

class RedisHandle final {
public:
    RedisHandle(const RedisHandle&) = default;
    RedisHandle& operator=(const RedisHandle&) = delete;

    Task<RedisValue> command(std::initializer_list<std::string_view> args) const;
    Task<RedisValue> command(std::span<const std::string_view> args) const;

    Task<void> ping() const;
    Task<std::pmr::string> ping(std::string_view message) const;
    Task<std::optional<std::pmr::string>> get(std::string_view key) const;
    Task<std::pmr::vector<std::optional<std::pmr::string>>> mget(std::span<const std::string_view> keys) const;
    Task<void> set(std::string_view key, std::string_view value) const;
    Task<std::optional<std::pmr::string>> set(std::string_view key, std::string_view value, RedisSetOptions options) const;
    Task<void> mset(std::span<const std::pair<std::string_view, std::string_view>> items) const;
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
    Task<std::pmr::vector<std::optional<std::pmr::string>>> hmget(std::string_view key, std::span<const std::string_view> fields) const;
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
    Task<std::pmr::vector<std::pmr::string>> sunion(std::span<const std::string_view> keys) const;
    Task<std::pmr::vector<std::pmr::string>> sdiff(std::span<const std::string_view> keys) const;
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
    Task<std::optional<RedisKeyValue>> blpop(std::span<const std::string_view> keys, std::chrono::seconds timeout) const;
    Task<std::optional<RedisKeyValue>> brpop(std::span<const std::string_view> keys, std::chrono::seconds timeout) const;

    [[nodiscard]] RedisPipeline pipeline() const;
    [[nodiscard]] RedisTransaction transaction() const;

private:
    friend class detail::RedisRegistry;

    RedisHandle(
        detail::RedisPool& pool,
        std::pmr::memory_resource* resource,
        RequestMemory* requestMemory = nullptr) noexcept;

    detail::RedisPool& pool_;
    std::pmr::memory_resource* resource_;
    RequestMemory* requestMemory_{nullptr};
};

}  // namespace ruvia
