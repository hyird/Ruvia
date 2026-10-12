#pragma once

#include <cstdint>
#include <functional>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/web/detail/redis/redis_owned_command.h"
#include "ruvia/web/detail/redis/redis_utils.h"

namespace ruvia::detail {

class redis_pool;

struct redis_command_payload final {
    std::reference_wrapper<redis_pool> pool_;
    operation_options options_;
    std::pmr::vector<redis_owned_command> commands_;
};

// Owns the common single-use batch lifecycle and all ordinary command algorithms.
// Scope registration remains the responsibility of the enclosing capability.
class redis_command_batch final {
public:
    redis_command_batch(redis_pool& pool, operation_options options,
        std::pmr::memory_resource* resource) noexcept;
    redis_command_batch(const redis_command_batch&) = delete;
    redis_command_batch& operator=(const redis_command_batch&) = delete;
    redis_command_batch(redis_command_batch&& other) noexcept;
    redis_command_batch& operator=(redis_command_batch&&) = delete;

    void require_ready() const;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] redis_command_payload consume();
    void expire() noexcept;
    // User-supplied raw commands are validated against stateful and blocking
    // command names; the typed helpers below compose from constant command
    // words and skip that scan.
    redis_command_batch& command(std::span<const std::string_view> args);

    redis_command_batch& get(std::string_view key) {
        return trusted("GET", key);
    }

    redis_command_batch& set(std::string_view key, std::string_view value) {
        return trusted("SET", key, value);
    }

    redis_command_batch& get_del(std::string_view key) {
        return trusted("GETDEL", key);
    }

    redis_command_batch& append(std::string_view key, std::string_view value) {
        return trusted("APPEND", key, value);
    }

    redis_command_batch& strlen(std::string_view key) {
        return trusted("STRLEN", key);
    }

    redis_command_batch& del(std::string_view key) {
        return trusted("DEL", key);
    }

    redis_command_batch& unlink(std::string_view key) {
        return trusted("UNLINK", key);
    }

    redis_command_batch& exists(std::string_view key) {
        return trusted("EXISTS", key);
    }

    redis_command_batch& touch(std::string_view key) {
        return trusted("TOUCH", key);
    }

    redis_command_batch& type(std::string_view key) {
        return trusted("TYPE", key);
    }

    redis_command_batch& rename(std::string_view key, std::string_view new_key) {
        return trusted("RENAME", key, new_key);
    }

    redis_command_batch& rename_nx(std::string_view key, std::string_view new_key) {
        return trusted("RENAMENX", key, new_key);
    }

    redis_command_batch& incr(std::string_view key) {
        return trusted("INCR", key);
    }

    redis_command_batch& incr_by(std::string_view key, std::int64_t value) {
        require_ready();
        auto amount = redis_int_string(value, resource());
        return trusted("INCRBY", key, std::string_view(amount));
    }

    redis_command_batch& decr(std::string_view key) {
        return trusted("DECR", key);
    }

    redis_command_batch& decr_by(std::string_view key, std::int64_t value) {
        require_ready();
        auto amount = redis_int_string(value, resource());
        return trusted("DECRBY", key, std::string_view(amount));
    }

    redis_command_batch& hget(std::string_view key, std::string_view field) {
        return trusted("HGET", key, field);
    }

    redis_command_batch& hset(std::string_view key, std::string_view field, std::string_view value) {
        return trusted("HSET", key, field, value);
    }

    redis_command_batch& hdel(std::string_view key, std::string_view field) {
        return trusted("HDEL", key, field);
    }

    redis_command_batch& hexists(std::string_view key, std::string_view field) {
        return trusted("HEXISTS", key, field);
    }

    redis_command_batch& hlen(std::string_view key) {
        return trusted("HLEN", key);
    }

    redis_command_batch& hget_all(std::string_view key) {
        return trusted("HGETALL", key);
    }

    redis_command_batch& lpush(std::string_view key, std::string_view value) {
        return trusted("LPUSH", key, value);
    }

    redis_command_batch& rpush(std::string_view key, std::string_view value) {
        return trusted("RPUSH", key, value);
    }

    redis_command_batch& lpop(std::string_view key) {
        return trusted("LPOP", key);
    }

    redis_command_batch& rpop(std::string_view key) {
        return trusted("RPOP", key);
    }

    redis_command_batch& llen(std::string_view key) {
        return trusted("LLEN", key);
    }

    redis_command_batch& lrange(std::string_view key, std::int64_t start, std::int64_t stop) {
        require_ready();
        auto start_value = redis_int_string(start, resource());
        auto stop_value = redis_int_string(stop, resource());
        return trusted(
            "LRANGE", key, std::string_view(start_value), std::string_view(stop_value));
    }

    redis_command_batch& sadd(std::string_view key, std::string_view member) {
        return trusted("SADD", key, member);
    }

    redis_command_batch& srem(std::string_view key, std::string_view member) {
        return trusted("SREM", key, member);
    }

    redis_command_batch& smembers(std::string_view key) {
        return trusted("SMEMBERS", key);
    }

    redis_command_batch& scard(std::string_view key) {
        return trusted("SCARD", key);
    }

    redis_command_batch& zadd(std::string_view key, double score, std::string_view member) {
        require_ready();
        auto score_value = redis_score_string(score, resource());
        return trusted("ZADD", key, std::string_view(score_value), member);
    }

    redis_command_batch& zrem(std::string_view key, std::string_view member) {
        return trusted("ZREM", key, member);
    }

    redis_command_batch& zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
        require_ready();
        auto start_value = redis_int_string(start, resource());
        auto stop_value = redis_int_string(stop, resource());
        return trusted(
            "ZRANGE", key, std::string_view(start_value), std::string_view(stop_value));
    }

    redis_command_batch& zscore(std::string_view key, std::string_view member) {
        return trusted("ZSCORE", key, member);
    }

    redis_command_batch& zcard(std::string_view key) {
        return trusted("ZCARD", key);
    }

private:
    redis_command_batch& append_trusted(std::string_view command_name, std::span<const std::string_view> rest);

    template <typename... args_type>
    redis_command_batch& trusted(std::string_view command_name, args_type... args) {
        const std::string_view rest[]{args...};
        return append_trusted(command_name, std::span<const std::string_view>(rest));
    }

    redis_pool* pool_;
    operation_options options_;
    std::pmr::vector<redis_owned_command> commands_;
};

}  // namespace ruvia::detail
