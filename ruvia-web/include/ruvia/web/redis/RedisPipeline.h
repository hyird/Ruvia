#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/web/redis/RedisTypes.h"

#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia {

class RedisHandle;
class RedisTransaction;

class RedisPipeline final {
public:
    RedisPipeline(const RedisPipeline&) = delete;
    RedisPipeline& operator=(const RedisPipeline&) = delete;
    RedisPipeline(RedisPipeline&& other) noexcept;
    RedisPipeline& operator=(RedisPipeline&&) = delete;

    RedisPipeline& command(std::span<const std::string_view> args);
    RedisPipeline& command(std::initializer_list<std::string_view> args) = delete;
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

    // Consumes the batch before returning the lazy Task, so the coroutine frame
    // owns every command and never borrows this builder through `this`.
    Task<std::pmr::vector<RedisValue>> exec() &&;

private:
    friend class RedisHandle;
    friend class RedisTransaction;
    friend class detail::RedisPool;

    struct Command final {
        std::pmr::vector<std::pmr::string> args;
    };

    [[nodiscard]] static Command makeCommand(
        std::pmr::memory_resource* resource,
        std::span<const std::string_view> args);
    [[nodiscard]] static Command makeCommand(
        std::pmr::memory_resource* resource,
        std::string_view first,
        std::span<const std::string_view> rest = {});
    static void appendCommand(
        std::pmr::vector<Command>& target,
        std::pmr::memory_resource* resource,
        std::span<const std::string_view> args);
    static void appendCommand(
        std::pmr::vector<Command>& target,
        std::pmr::memory_resource* resource,
        std::string_view first,
        std::span<const std::string_view> rest = {});

    RedisPipeline(
        detail::RedisPool& pool,
        std::pmr::memory_resource* resource) noexcept;
    [[nodiscard]] static Task<std::pmr::vector<RedisValue>> executeOwned(
        detail::RedisPool& pool,
        std::pmr::vector<Command> commands,
        std::pmr::memory_resource* resource);
    void requireActive() const;

    detail::RedisPool* pool_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    std::pmr::vector<Command> commands_;
};

}  // namespace ruvia
