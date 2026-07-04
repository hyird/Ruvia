#pragma once

#include "ruvia/redis/RedisPipeline.h"

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

namespace ruvia {

class RedisTransaction final {
public:
    RedisTransaction(const RedisTransaction&) = delete;
    RedisTransaction& operator=(const RedisTransaction&) = delete;
    RedisTransaction(RedisTransaction&&) noexcept = default;
    RedisTransaction& operator=(RedisTransaction&&) noexcept = default;

    RedisTransaction& command(std::span<const std::string_view> args);
    RedisTransaction& command(std::initializer_list<std::string_view> args) = delete;
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
    RedisTransaction& markActive() noexcept;

    RedisPipeline pipeline_;
    std::pmr::vector<RedisPipeline::Command> watches_;
    bool discarded_{false};
};

}  // namespace ruvia
