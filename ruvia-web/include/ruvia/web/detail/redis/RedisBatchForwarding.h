#pragma once

#include <cstdint>
#include <string_view>

// Fixed thin fluent forwarding; command algorithms live only in redis_command_batch.
#define RUVIA_REDIS_BATCH_FORWARDING(owner_type)                                             \
    owner_type& get(std::string_view key) {                                                  \
        registration_.require_active();                                                      \
        batch_.get(key);                                                                     \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& set(std::string_view key, std::string_view value) {                          \
        registration_.require_active();                                                      \
        batch_.set(key, value);                                                              \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& getDel(std::string_view key) {                                               \
        registration_.require_active();                                                      \
        batch_.get_del(key);                                                                 \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& append(std::string_view key, std::string_view value) {                       \
        registration_.require_active();                                                      \
        batch_.append(key, value);                                                           \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& strlen(std::string_view key) {                                               \
        registration_.require_active();                                                      \
        batch_.strlen(key);                                                                  \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& del(std::string_view key) {                                                  \
        registration_.require_active();                                                      \
        batch_.del(key);                                                                     \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& unlink(std::string_view key) {                                               \
        registration_.require_active();                                                      \
        batch_.unlink(key);                                                                  \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& exists(std::string_view key) {                                               \
        registration_.require_active();                                                      \
        batch_.exists(key);                                                                  \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& touch(std::string_view key) {                                                \
        registration_.require_active();                                                      \
        batch_.touch(key);                                                                   \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& type(std::string_view key) {                                                 \
        registration_.require_active();                                                      \
        batch_.type(key);                                                                    \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& rename(std::string_view key, std::string_view new_key) {                     \
        registration_.require_active();                                                      \
        batch_.rename(key, new_key);                                                         \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& renameNx(std::string_view key, std::string_view new_key) {                   \
        registration_.require_active();                                                      \
        batch_.rename_nx(key, new_key);                                                      \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& incr(std::string_view key) {                                                 \
        registration_.require_active();                                                      \
        batch_.incr(key);                                                                    \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& incrBy(std::string_view key, std::int64_t value) {                           \
        registration_.require_active();                                                      \
        batch_.incr_by(key, value);                                                          \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& decr(std::string_view key) {                                                 \
        registration_.require_active();                                                      \
        batch_.decr(key);                                                                    \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& decrBy(std::string_view key, std::int64_t value) {                           \
        registration_.require_active();                                                      \
        batch_.decr_by(key, value);                                                          \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& hget(std::string_view key, std::string_view field) {                         \
        registration_.require_active();                                                      \
        batch_.hget(key, field);                                                             \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& hset(std::string_view key, std::string_view field, std::string_view value) { \
        registration_.require_active();                                                      \
        batch_.hset(key, field, value);                                                      \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& hdel(std::string_view key, std::string_view field) {                         \
        registration_.require_active();                                                      \
        batch_.hdel(key, field);                                                             \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& hexists(std::string_view key, std::string_view field) {                      \
        registration_.require_active();                                                      \
        batch_.hexists(key, field);                                                          \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& hlen(std::string_view key) {                                                 \
        registration_.require_active();                                                      \
        batch_.hlen(key);                                                                    \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& hgetAll(std::string_view key) {                                              \
        registration_.require_active();                                                      \
        batch_.hget_all(key);                                                                \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& lpush(std::string_view key, std::string_view value) {                        \
        registration_.require_active();                                                      \
        batch_.lpush(key, value);                                                            \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& rpush(std::string_view key, std::string_view value) {                        \
        registration_.require_active();                                                      \
        batch_.rpush(key, value);                                                            \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& lpop(std::string_view key) {                                                 \
        registration_.require_active();                                                      \
        batch_.lpop(key);                                                                    \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& rpop(std::string_view key) {                                                 \
        registration_.require_active();                                                      \
        batch_.rpop(key);                                                                    \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& llen(std::string_view key) {                                                 \
        registration_.require_active();                                                      \
        batch_.llen(key);                                                                    \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& lrange(std::string_view key, std::int64_t start, std::int64_t stop) {        \
        registration_.require_active();                                                      \
        batch_.lrange(key, start, stop);                                                     \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& sadd(std::string_view key, std::string_view member) {                        \
        registration_.require_active();                                                      \
        batch_.sadd(key, member);                                                            \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& srem(std::string_view key, std::string_view member) {                        \
        registration_.require_active();                                                      \
        batch_.srem(key, member);                                                            \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& smembers(std::string_view key) {                                             \
        registration_.require_active();                                                      \
        batch_.smembers(key);                                                                \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& scard(std::string_view key) {                                                \
        registration_.require_active();                                                      \
        batch_.scard(key);                                                                   \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& zadd(std::string_view key, double score, std::string_view member) {          \
        registration_.require_active();                                                      \
        batch_.zadd(key, score, member);                                                     \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& zrem(std::string_view key, std::string_view member) {                        \
        registration_.require_active();                                                      \
        batch_.zrem(key, member);                                                            \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& zrange(std::string_view key, std::int64_t start, std::int64_t stop) {        \
        registration_.require_active();                                                      \
        batch_.zrange(key, start, stop);                                                     \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& zscore(std::string_view key, std::string_view member) {                      \
        registration_.require_active();                                                      \
        batch_.zscore(key, member);                                                          \
        return *this;                                                                        \
    }                                                                                        \
    owner_type& zcard(std::string_view key) {                                                \
        registration_.require_active();                                                      \
        batch_.zcard(key);                                                                   \
        return *this;                                                                        \
    }
