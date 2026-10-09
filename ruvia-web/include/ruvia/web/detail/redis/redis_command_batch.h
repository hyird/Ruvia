#pragma once

#include <cstdint>
#include <functional>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/web/detail/redis/redis_argument_pack.h"
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
    redis_command_batch& command(std::span<const std::string_view> args);

    template <typename... args_type>
        requires redis_argument_pack<args_type...>
    redis_command_batch& command(args_type&&... args) {
        const std::string_view views[]{std::string_view(args)...};
        return command(std::span<const std::string_view>(views));
    }

    redis_command_batch& get(std::string_view key) {
        return command("GET", key);
    }

    redis_command_batch& set(std::string_view key, std::string_view value) {
        return command("SET", key, value);
    }

    redis_command_batch& get_del(std::string_view key) {
        return command("GETDEL", key);
    }

    redis_command_batch& append(std::string_view key, std::string_view value) {
        return command("APPEND", key, value);
    }

    redis_command_batch& strlen(std::string_view key) {
        return command("STRLEN", key);
    }

    redis_command_batch& del(std::string_view key) {
        return command("DEL", key);
    }

    redis_command_batch& unlink(std::string_view key) {
        return command("UNLINK", key);
    }

    redis_command_batch& exists(std::string_view key) {
        return command("EXISTS", key);
    }

    redis_command_batch& touch(std::string_view key) {
        return command("TOUCH", key);
    }

    redis_command_batch& type(std::string_view key) {
        return command("TYPE", key);
    }

    redis_command_batch& rename(std::string_view key, std::string_view new_key) {
        return command("RENAME", key, new_key);
    }

    redis_command_batch& rename_nx(std::string_view key, std::string_view new_key) {
        return command("RENAMENX", key, new_key);
    }

    redis_command_batch& incr(std::string_view key) {
        return command("INCR", key);
    }

    redis_command_batch& incr_by(std::string_view key, std::int64_t value) {
        require_ready();
        auto amount = redis_int_string(value, resource());
        return command("INCRBY", key, std::string_view(amount));
    }

    redis_command_batch& decr(std::string_view key) {
        return command("DECR", key);
    }

    redis_command_batch& decr_by(std::string_view key, std::int64_t value) {
        require_ready();
        auto amount = redis_int_string(value, resource());
        return command("DECRBY", key, std::string_view(amount));
    }

    redis_command_batch& hget(std::string_view key, std::string_view field) {
        return command("HGET", key, field);
    }

    redis_command_batch& hset(std::string_view key, std::string_view field, std::string_view value) {
        return command("HSET", key, field, value);
    }

    redis_command_batch& hdel(std::string_view key, std::string_view field) {
        return command("HDEL", key, field);
    }

    redis_command_batch& hexists(std::string_view key, std::string_view field) {
        return command("HEXISTS", key, field);
    }

    redis_command_batch& hlen(std::string_view key) {
        return command("HLEN", key);
    }

    redis_command_batch& hget_all(std::string_view key) {
        return command("HGETALL", key);
    }

    redis_command_batch& lpush(std::string_view key, std::string_view value) {
        return command("LPUSH", key, value);
    }

    redis_command_batch& rpush(std::string_view key, std::string_view value) {
        return command("RPUSH", key, value);
    }

    redis_command_batch& lpop(std::string_view key) {
        return command("LPOP", key);
    }

    redis_command_batch& rpop(std::string_view key) {
        return command("RPOP", key);
    }

    redis_command_batch& llen(std::string_view key) {
        return command("LLEN", key);
    }

    redis_command_batch& lrange(std::string_view key, std::int64_t start, std::int64_t stop) {
        require_ready();
        auto start_value = redis_int_string(start, resource());
        auto stop_value = redis_int_string(stop, resource());
        return command(
            "LRANGE", key, std::string_view(start_value), std::string_view(stop_value));
    }

    redis_command_batch& sadd(std::string_view key, std::string_view member) {
        return command("SADD", key, member);
    }

    redis_command_batch& srem(std::string_view key, std::string_view member) {
        return command("SREM", key, member);
    }

    redis_command_batch& smembers(std::string_view key) {
        return command("SMEMBERS", key);
    }

    redis_command_batch& scard(std::string_view key) {
        return command("SCARD", key);
    }

    redis_command_batch& zadd(std::string_view key, double score, std::string_view member) {
        require_ready();
        auto score_value = redis_score_string(score, resource());
        return command("ZADD", key, std::string_view(score_value), member);
    }

    redis_command_batch& zrem(std::string_view key, std::string_view member) {
        return command("ZREM", key, member);
    }

    redis_command_batch& zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
        require_ready();
        auto start_value = redis_int_string(start, resource());
        auto stop_value = redis_int_string(stop, resource());
        return command(
            "ZRANGE", key, std::string_view(start_value), std::string_view(stop_value));
    }

    redis_command_batch& zscore(std::string_view key, std::string_view member) {
        return command("ZSCORE", key, member);
    }

    redis_command_batch& zcard(std::string_view key) {
        return command("ZCARD", key);
    }

private:
    redis_pool* pool_;
    operation_options options_;
    std::pmr::vector<redis_owned_command> commands_;
};

}  // namespace ruvia::detail
