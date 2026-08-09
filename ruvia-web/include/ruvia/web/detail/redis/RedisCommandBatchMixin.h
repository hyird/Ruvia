#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/web/detail/redis/RedisUtils.h"

namespace ruvia::detail {

// The typed command-word surface (get/set/hget/...) shared by RedisPipeline and
// RedisTransaction. Each entry appends one Redis command through the derived
// class's variadic command() entry point, so a new command word is written once
// here instead of being duplicated in both batch types.
//
// Derived must declare `friend class detail::RedisCommandBatchMixin<Derived>;`
// so the numeric entries can reach its private requireActive()/resource().
template <typename Derived>
class RedisCommandBatchMixin {
protected:
    ~RedisCommandBatchMixin() = default;

public:
    Derived& get(std::string_view key) {
        return self().command("GET", key);
    }

    Derived& set(std::string_view key, std::string_view value) {
        return self().command("SET", key, value);
    }

    Derived& getDel(std::string_view key) {
        return self().command("GETDEL", key);
    }

    Derived& getSet(std::string_view key, std::string_view value) {
        return self().command("GETSET", key, value);
    }

    Derived& append(std::string_view key, std::string_view value) {
        return self().command("APPEND", key, value);
    }

    Derived& strlen(std::string_view key) {
        return self().command("STRLEN", key);
    }

    Derived& del(std::string_view key) {
        return self().command("DEL", key);
    }

    Derived& unlink(std::string_view key) {
        return self().command("UNLINK", key);
    }

    Derived& exists(std::string_view key) {
        return self().command("EXISTS", key);
    }

    Derived& touch(std::string_view key) {
        return self().command("TOUCH", key);
    }

    Derived& type(std::string_view key) {
        return self().command("TYPE", key);
    }

    Derived& rename(std::string_view key, std::string_view newKey) {
        return self().command("RENAME", key, newKey);
    }

    Derived& renameNx(std::string_view key, std::string_view newKey) {
        return self().command("RENAMENX", key, newKey);
    }

    Derived& incr(std::string_view key) {
        return self().command("INCR", key);
    }

    Derived& incrBy(std::string_view key, std::int64_t value) {
        self().requireActive();
        auto amount = redisIntString(value, self().resource());
        return self().command("INCRBY", key, std::string_view(amount));
    }

    Derived& decr(std::string_view key) {
        return self().command("DECR", key);
    }

    Derived& decrBy(std::string_view key, std::int64_t value) {
        self().requireActive();
        auto amount = redisIntString(value, self().resource());
        return self().command("DECRBY", key, std::string_view(amount));
    }

    Derived& hget(std::string_view key, std::string_view field) {
        return self().command("HGET", key, field);
    }

    Derived& hset(std::string_view key, std::string_view field, std::string_view value) {
        return self().command("HSET", key, field, value);
    }

    Derived& hdel(std::string_view key, std::string_view field) {
        return self().command("HDEL", key, field);
    }

    Derived& hexists(std::string_view key, std::string_view field) {
        return self().command("HEXISTS", key, field);
    }

    Derived& hlen(std::string_view key) {
        return self().command("HLEN", key);
    }

    Derived& hgetAll(std::string_view key) {
        return self().command("HGETALL", key);
    }

    Derived& lpush(std::string_view key, std::string_view value) {
        return self().command("LPUSH", key, value);
    }

    Derived& rpush(std::string_view key, std::string_view value) {
        return self().command("RPUSH", key, value);
    }

    Derived& lpop(std::string_view key) {
        return self().command("LPOP", key);
    }

    Derived& rpop(std::string_view key) {
        return self().command("RPOP", key);
    }

    Derived& llen(std::string_view key) {
        return self().command("LLEN", key);
    }

    Derived& lrange(std::string_view key, std::int64_t start, std::int64_t stop) {
        self().requireActive();
        auto startValue = redisIntString(start, self().resource());
        auto stopValue = redisIntString(stop, self().resource());
        return self().command("LRANGE", key, std::string_view(startValue), std::string_view(stopValue));
    }

    Derived& sadd(std::string_view key, std::string_view member) {
        return self().command("SADD", key, member);
    }

    Derived& srem(std::string_view key, std::string_view member) {
        return self().command("SREM", key, member);
    }

    Derived& smembers(std::string_view key) {
        return self().command("SMEMBERS", key);
    }

    Derived& scard(std::string_view key) {
        return self().command("SCARD", key);
    }

    Derived& zadd(std::string_view key, double score, std::string_view member) {
        self().requireActive();
        auto scoreValue = redisScoreString(score, self().resource());
        return self().command("ZADD", key, std::string_view(scoreValue), member);
    }

    Derived& zrem(std::string_view key, std::string_view member) {
        return self().command("ZREM", key, member);
    }

    Derived& zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
        self().requireActive();
        auto startValue = redisIntString(start, self().resource());
        auto stopValue = redisIntString(stop, self().resource());
        return self().command("ZRANGE", key, std::string_view(startValue), std::string_view(stopValue));
    }

    Derived& zscore(std::string_view key, std::string_view member) {
        return self().command("ZSCORE", key, member);
    }

    Derived& zcard(std::string_view key) {
        return self().command("ZCARD", key);
    }

private:
    Derived& self() noexcept {
        return static_cast<Derived&>(*this);
    }
};

}  // namespace ruvia::detail
