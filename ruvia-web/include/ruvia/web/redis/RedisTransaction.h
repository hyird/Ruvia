#pragma once

#include "ruvia/web/redis/RedisPipeline.h"
#include "ruvia/web/detail/redis/RedisArgumentPack.h"

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

namespace ruvia {

class RedisTransaction final : private detail::ScopedCapabilityNode {
public:
    RedisTransaction(const RedisTransaction&) = delete;
    RedisTransaction& operator=(const RedisTransaction&) = delete;
    RedisTransaction(RedisTransaction&&) noexcept = default;
    RedisTransaction& operator=(RedisTransaction&&) = delete;

    RedisTransaction& command(std::span<const std::string_view> args);
    RedisTransaction& command(std::initializer_list<std::string_view> args) = delete;
    RedisTransaction& watch(std::string_view key);
    RedisTransaction& watch(std::span<const std::string_view> keys);

    // As on RedisPipeline: command words and watched keys as ordinary arguments,
    // copied into the batch before the call returns. Two or more keys, so a
    // single-key watch(key) keeps its own overload.
    template <typename... Args>
        requires detail::RedisArgumentPack<Args...>
    RedisTransaction& command(Args&&... args) {
        const std::string_view views[]{std::string_view(args)...};
        return command(std::span<const std::string_view>(views));
    }

    template <typename... Keys>
        requires(detail::RedisArgumentPack<Keys...> && sizeof...(Keys) >= 2)
    RedisTransaction& watch(Keys&&... keys) {
        const std::string_view views[]{std::string_view(keys)...};
        return watch(std::span<const std::string_view>(views));
    }

    RedisTransaction& unwatch();
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

    // A transaction is a single-use command batch. Its commands are transferred
    // into the returned coroutine frame before this builder may be destroyed.
    ScopedOperation<std::pmr::vector<RedisValue>> exec() &&;
    ScopedOperation<std::pmr::vector<RedisValue>> exec(RedisOperationOptions options) &&;

private:
    friend class RedisHandle;

    explicit RedisTransaction(RedisPipeline pipeline) noexcept;
    [[nodiscard]] static Task<std::pmr::vector<RedisValue>> executeOwned(detail::RedisPool& pool, RedisOperationOptions options, std::pmr::memory_resource* resource, std::pmr::vector<RedisPipeline::Command> watches, std::pmr::vector<RedisPipeline::Command> commands);

    RedisPipeline pipeline_;
    std::pmr::vector<RedisPipeline::Command> watches_;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;
};

}  // namespace ruvia
