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

#include "ruvia/core/event_loop.h"
#include "ruvia/web/redis/redis_handle.h"
#include "ruvia/web/redis/redis_repository.h"

namespace ruvia {
namespace detail {
class redis_client_state;
}

// One Redis client bound to one event_loop, independent of HTTP and application.
// Handles, repositories, cold operations and PMR results borrow this owner.
// Destroy them on the bound loop before destroying the client; shutdown joins
// started operations but does not extend the allocator lifetime of results.
class redis_client final {
public:
    redis_client(event_loop loop, const redis_config& config);
    ~redis_client();
    redis_client(const redis_client&) = delete;
    redis_client& operator=(const redis_client&) = delete;
    redis_client(redis_client&&) = delete;
    redis_client& operator=(redis_client&&) = delete;

    // Lazy, single-start and worker-affine, like ordinary commands.
    [[nodiscard]] task<void> connect() &;
    task<void> connect() && = delete;
    [[nodiscard]] redis_handle with_options(operation_options options) const&;
    redis_handle with_options(operation_options) const&& = delete;

    template <typename entity_type>
    [[nodiscard]] redis_repository<entity_type> get_repository(
        const redis_repository_config& config = {}) const& {
        return with_options({}).template get_repository<entity_type>(config);
    }
    template <typename entity_type>
    redis_repository<entity_type> get_repository(const redis_repository_config& = {}) const&& = delete;

    [[nodiscard]] redis_pipeline pipeline() const& {
        return with_options({}).pipeline();
    }
    redis_pipeline pipeline() const&& = delete;
    [[nodiscard]] redis_transaction transaction() const& {
        return with_options({}).transaction();
    }
    redis_transaction transaction() const&& = delete;

    scoped_operation<redis_value> command(std::span<const std::string_view> args) const& {
        return with_options({}).command(args);
    }

    scoped_operation<redis_value> command(std::span<const std::string_view> args) const&& = delete;

    scoped_operation<redis_value> command(
        std::initializer_list<std::string_view> args) const& = delete;

    scoped_operation<redis_value> command(
        std::initializer_list<std::string_view> args) const&& = delete;

    scoped_operation<void> ping() const& {
        return with_options({}).ping();
    }

    scoped_operation<void> ping() const&& = delete;

    scoped_operation<std::pmr::string> ping(std::string_view message) const& {
        return with_options({}).ping(message);
    }

    scoped_operation<std::pmr::string> ping(std::string_view message) const&& = delete;

    scoped_operation<std::optional<std::pmr::string>> get(std::string_view key) const& {
        return with_options({}).get(key);
    }

    scoped_operation<std::optional<std::pmr::string>> get(std::string_view key) const&& = delete;

    scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        std::span<const std::string_view> keys) const& {
        return with_options({}).mget(keys);
    }

    scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        std::span<const std::string_view> keys) const&& = delete;

    scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        std::initializer_list<std::string_view> keys) const& = delete;

    scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        std::initializer_list<std::string_view> keys) const&& = delete;

    scoped_operation<redis_set_result> set(
        std::string_view key, std::string_view value, redis_set_options options = {}) const& {
        return with_options({}).set(key, value, options);
    }

    scoped_operation<redis_set_result> set(
        std::string_view key, std::string_view value, redis_set_options options = {}) const&& = delete;

    scoped_operation<void> mset(
        std::span<const std::pair<std::string_view, std::string_view>> items) const& {
        return with_options({}).mset(items);
    }

    scoped_operation<void> mset(
        std::span<const std::pair<std::string_view, std::string_view>> items) const&& = delete;

    scoped_operation<void> mset(
        std::initializer_list<std::pair<std::string_view, std::string_view>> items) const& = delete;

    scoped_operation<void> mset(
        std::initializer_list<std::pair<std::string_view, std::string_view>> items) const&& = delete;

    scoped_operation<std::optional<std::pmr::string>> get_del(std::string_view key) const& {
        return with_options({}).get_del(key);
    }

    scoped_operation<std::optional<std::pmr::string>> get_del(std::string_view key) const&& = delete;

    scoped_operation<std::int64_t> append(std::string_view key, std::string_view value) const& {
        return with_options({}).append(key, value);
    }

    scoped_operation<std::int64_t> append(std::string_view key, std::string_view value) const&& = delete;

    scoped_operation<std::int64_t> strlen(std::string_view key) const& {
        return with_options({}).strlen(key);
    }

    scoped_operation<std::int64_t> strlen(std::string_view key) const&& = delete;

    scoped_operation<std::int64_t> incr_by(std::string_view key, std::int64_t value) const& {
        return with_options({}).incr_by(key, value);
    }

    scoped_operation<std::int64_t> incr_by(std::string_view key, std::int64_t value) const&& = delete;

    scoped_operation<std::int64_t> decr(std::string_view key) const& {
        return with_options({}).decr(key);
    }

    scoped_operation<std::int64_t> decr(std::string_view key) const&& = delete;

    scoped_operation<std::int64_t> decr_by(std::string_view key, std::int64_t value) const& {
        return with_options({}).decr_by(key, value);
    }

    scoped_operation<std::int64_t> decr_by(std::string_view key, std::int64_t value) const&& = delete;

    scoped_operation<std::int64_t> del(std::string_view key) const& {
        return with_options({}).del(key);
    }

    scoped_operation<std::int64_t> del(std::string_view key) const&& = delete;

    scoped_operation<std::int64_t> unlink(std::string_view key) const& {
        return with_options({}).unlink(key);
    }

    scoped_operation<std::int64_t> unlink(std::string_view key) const&& = delete;

    scoped_operation<bool> exists(std::string_view key) const& {
        return with_options({}).exists(key);
    }

    scoped_operation<bool> exists(std::string_view key) const&& = delete;

    scoped_operation<bool> touch(std::string_view key) const& {
        return with_options({}).touch(key);
    }

    scoped_operation<bool> touch(std::string_view key) const&& = delete;

    scoped_operation<std::pmr::string> type(std::string_view key) const& {
        return with_options({}).type(key);
    }

    scoped_operation<std::pmr::string> type(std::string_view key) const&& = delete;

    scoped_operation<void> rename(std::string_view key, std::string_view new_key) const& {
        return with_options({}).rename(key, new_key);
    }

    scoped_operation<void> rename(std::string_view key, std::string_view new_key) const&& = delete;

    scoped_operation<bool> rename_nx(std::string_view key, std::string_view new_key) const& {
        return with_options({}).rename_nx(key, new_key);
    }

    scoped_operation<bool> rename_nx(std::string_view key, std::string_view new_key) const&& = delete;

    scoped_operation<bool> expire(std::string_view key, std::chrono::seconds ttl) const& {
        return with_options({}).expire(key, ttl);
    }

    scoped_operation<bool> expire(std::string_view key, std::chrono::seconds ttl) const&& = delete;

    scoped_operation<bool> expire_at(
        std::string_view key, std::chrono::system_clock::time_point expires_at) const& {
        return with_options({}).expire_at(key, expires_at);
    }

    scoped_operation<bool> expire_at(
        std::string_view key, std::chrono::system_clock::time_point expires_at) const&& = delete;

    scoped_operation<bool> persist(std::string_view key) const& {
        return with_options({}).persist(key);
    }

    scoped_operation<bool> persist(std::string_view key) const&& = delete;

    scoped_operation<redis_ttl> ttl(std::string_view key) const& {
        return with_options({}).ttl(key);
    }

    scoped_operation<redis_ttl> ttl(std::string_view key) const&& = delete;

    scoped_operation<redis_ttl> pttl(std::string_view key) const& {
        return with_options({}).pttl(key);
    }

    scoped_operation<redis_ttl> pttl(std::string_view key) const&& = delete;

    scoped_operation<std::int64_t> incr(std::string_view key) const& {
        return with_options({}).incr(key);
    }

    scoped_operation<std::int64_t> incr(std::string_view key) const&& = delete;

    scoped_operation<std::optional<std::pmr::string>> hget(
        std::string_view key, std::string_view field) const& {
        return with_options({}).hget(key, field);
    }

    scoped_operation<std::optional<std::pmr::string>> hget(
        std::string_view key, std::string_view field) const&& = delete;

    scoped_operation<std::int64_t> hset(
        std::string_view key, std::string_view field, std::string_view value) const& {
        return with_options({}).hset(key, field, value);
    }

    scoped_operation<std::int64_t> hset(
        std::string_view key, std::string_view field, std::string_view value) const&& = delete;

    scoped_operation<std::int64_t> hset(std::string_view key,
        std::span<const std::pair<std::string_view, std::string_view>> fields_value) const& {
        return with_options({}).hset(key, fields_value);
    }

    scoped_operation<std::int64_t> hset(std::string_view key,
        std::span<const std::pair<std::string_view, std::string_view>> fields_value) const&& = delete;

    scoped_operation<std::int64_t> hset(std::string_view key,
        std::initializer_list<std::pair<std::string_view, std::string_view>> fields_value) const& = delete;

    scoped_operation<std::int64_t> hset(std::string_view key,
        std::initializer_list<std::pair<std::string_view, std::string_view>> fields_value) const&& = delete;

    scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, std::span<const std::string_view> fields_value) const& {
        return with_options({}).hmget(key, fields_value);
    }

    scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, std::span<const std::string_view> fields_value) const&& = delete;

    scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, std::initializer_list<std::string_view> fields_value) const& = delete;

    scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, std::initializer_list<std::string_view> fields_value) const&& = delete;

    scoped_operation<std::pmr::vector<redis_key_value>> hget_all(std::string_view key) const& {
        return with_options({}).hget_all(key);
    }

    scoped_operation<std::pmr::vector<redis_key_value>> hget_all(std::string_view key) const&& = delete;

    scoped_operation<std::int64_t> hdel(std::string_view key, std::string_view field) const& {
        return with_options({}).hdel(key, field);
    }

    scoped_operation<std::int64_t> hdel(std::string_view key, std::string_view field) const&& = delete;

    scoped_operation<bool> hexists(std::string_view key, std::string_view field) const& {
        return with_options({}).hexists(key, field);
    }

    scoped_operation<bool> hexists(std::string_view key, std::string_view field) const&& = delete;

    scoped_operation<std::int64_t> hlen(std::string_view key) const& {
        return with_options({}).hlen(key);
    }

    scoped_operation<std::int64_t> hlen(std::string_view key) const&& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> hkeys(std::string_view key) const& {
        return with_options({}).hkeys(key);
    }

    scoped_operation<std::pmr::vector<std::pmr::string>> hkeys(std::string_view key) const&& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> hvals(std::string_view key) const& {
        return with_options({}).hvals(key);
    }

    scoped_operation<std::pmr::vector<std::pmr::string>> hvals(std::string_view key) const&& = delete;

    scoped_operation<std::int64_t> hincr_by(
        std::string_view key, std::string_view field, std::int64_t value) const& {
        return with_options({}).hincr_by(key, field, value);
    }

    scoped_operation<std::int64_t> hincr_by(
        std::string_view key, std::string_view field, std::int64_t value) const&& = delete;

    scoped_operation<std::int64_t> lpush(std::string_view key, std::string_view value) const& {
        return with_options({}).lpush(key, value);
    }

    scoped_operation<std::int64_t> lpush(std::string_view key, std::string_view value) const&& = delete;

    scoped_operation<std::int64_t> rpush(std::string_view key, std::string_view value) const& {
        return with_options({}).rpush(key, value);
    }

    scoped_operation<std::int64_t> rpush(std::string_view key, std::string_view value) const&& = delete;

    scoped_operation<std::optional<std::pmr::string>> lpop(std::string_view key) const& {
        return with_options({}).lpop(key);
    }

    scoped_operation<std::optional<std::pmr::string>> lpop(std::string_view key) const&& = delete;

    scoped_operation<std::optional<std::pmr::string>> rpop(std::string_view key) const& {
        return with_options({}).rpop(key);
    }

    scoped_operation<std::optional<std::pmr::string>> rpop(std::string_view key) const&& = delete;

    scoped_operation<std::int64_t> llen(std::string_view key) const& {
        return with_options({}).llen(key);
    }

    scoped_operation<std::int64_t> llen(std::string_view key) const&& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> lrange(
        std::string_view key, std::int64_t start, std::int64_t stop) const& {
        return with_options({}).lrange(key, start, stop);
    }

    scoped_operation<std::pmr::vector<std::pmr::string>> lrange(
        std::string_view key, std::int64_t start, std::int64_t stop) const&& = delete;

    scoped_operation<std::optional<std::pmr::string>> lindex(
        std::string_view key, std::int64_t index) const& {
        return with_options({}).lindex(key, index);
    }

    scoped_operation<std::optional<std::pmr::string>> lindex(
        std::string_view key, std::int64_t index) const&& = delete;

    scoped_operation<void> lset(
        std::string_view key, std::int64_t index, std::string_view value) const& {
        return with_options({}).lset(key, index, value);
    }

    scoped_operation<void> lset(
        std::string_view key, std::int64_t index, std::string_view value) const&& = delete;

    scoped_operation<void> ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const& {
        return with_options({}).ltrim(key, start, stop);
    }

    scoped_operation<void> ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const&& = delete;

    scoped_operation<std::int64_t> lrem(
        std::string_view key, std::int64_t count, std::string_view value) const& {
        return with_options({}).lrem(key, count, value);
    }

    scoped_operation<std::int64_t> lrem(
        std::string_view key, std::int64_t count, std::string_view value) const&& = delete;

    scoped_operation<std::int64_t> sadd(std::string_view key, std::string_view member) const& {
        return with_options({}).sadd(key, member);
    }

    scoped_operation<std::int64_t> sadd(std::string_view key, std::string_view member) const&& = delete;

    scoped_operation<std::int64_t> srem(std::string_view key, std::string_view member) const& {
        return with_options({}).srem(key, member);
    }

    scoped_operation<std::int64_t> srem(std::string_view key, std::string_view member) const&& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> smembers(std::string_view key) const& {
        return with_options({}).smembers(key);
    }

    scoped_operation<std::pmr::vector<std::pmr::string>> smembers(std::string_view key) const&& = delete;

    scoped_operation<std::int64_t> scard(std::string_view key) const& {
        return with_options({}).scard(key);
    }

    scoped_operation<std::int64_t> scard(std::string_view key) const&& = delete;

    scoped_operation<bool> sismember(std::string_view key, std::string_view member) const& {
        return with_options({}).sismember(key, member);
    }

    scoped_operation<bool> sismember(std::string_view key, std::string_view member) const&& = delete;

    scoped_operation<std::optional<std::pmr::string>> spop(std::string_view key) const& {
        return with_options({}).spop(key);
    }

    scoped_operation<std::optional<std::pmr::string>> spop(std::string_view key) const&& = delete;

    scoped_operation<std::optional<std::pmr::string>> srand_member(std::string_view key) const& {
        return with_options({}).srand_member(key);
    }

    scoped_operation<std::optional<std::pmr::string>> srand_member(std::string_view key) const&& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> sinter(
        std::span<const std::string_view> keys) const& {
        return with_options({}).sinter(keys);
    }

    scoped_operation<std::pmr::vector<std::pmr::string>> sinter(
        std::span<const std::string_view> keys) const&& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> sinter(
        std::initializer_list<std::string_view> keys) const& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> sinter(
        std::initializer_list<std::string_view> keys) const&& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> sunion(
        std::span<const std::string_view> keys) const& {
        return with_options({}).sunion(keys);
    }

    scoped_operation<std::pmr::vector<std::pmr::string>> sunion(
        std::span<const std::string_view> keys) const&& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> sunion(
        std::initializer_list<std::string_view> keys) const& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> sunion(
        std::initializer_list<std::string_view> keys) const&& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> sdiff(
        std::span<const std::string_view> keys) const& {
        return with_options({}).sdiff(keys);
    }

    scoped_operation<std::pmr::vector<std::pmr::string>> sdiff(
        std::span<const std::string_view> keys) const&& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> sdiff(
        std::initializer_list<std::string_view> keys) const& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> sdiff(
        std::initializer_list<std::string_view> keys) const&& = delete;

    scoped_operation<std::int64_t> zadd(
        std::string_view key, double score, std::string_view member) const& {
        return with_options({}).zadd(key, score, member);
    }

    scoped_operation<std::int64_t> zadd(
        std::string_view key, double score, std::string_view member) const&& = delete;

    scoped_operation<std::int64_t> zrem(std::string_view key, std::string_view member) const& {
        return with_options({}).zrem(key, member);
    }

    scoped_operation<std::int64_t> zrem(std::string_view key, std::string_view member) const&& = delete;

    scoped_operation<std::pmr::vector<std::pmr::string>> zrange(
        std::string_view key, std::int64_t start, std::int64_t stop) const& {
        return with_options({}).zrange(key, start, stop);
    }

    scoped_operation<std::pmr::vector<std::pmr::string>> zrange(
        std::string_view key, std::int64_t start, std::int64_t stop) const&& = delete;

    scoped_operation<std::pmr::vector<redis_scored_value>> zrange_with_scores(
        std::string_view key, std::int64_t start, std::int64_t stop) const& {
        return with_options({}).zrange_with_scores(key, start, stop);
    }

    scoped_operation<std::pmr::vector<redis_scored_value>> zrange_with_scores(
        std::string_view key, std::int64_t start, std::int64_t stop) const&& = delete;

    scoped_operation<std::optional<double>> zscore(
        std::string_view key, std::string_view member) const& {
        return with_options({}).zscore(key, member);
    }

    scoped_operation<std::optional<double>> zscore(
        std::string_view key, std::string_view member) const&& = delete;

    scoped_operation<std::int64_t> zcard(std::string_view key) const& {
        return with_options({}).zcard(key);
    }

    scoped_operation<std::int64_t> zcard(std::string_view key) const&& = delete;

    scoped_operation<std::int64_t> zcount(std::string_view key, double min_value, double max_value) const& {
        return with_options({}).zcount(key, min_value, max_value);
    }

    scoped_operation<std::int64_t> zcount(std::string_view key, double min_value, double max_value) const&& = delete;

    scoped_operation<redis_scan_result> scan(redis_scan_options options = {}) const& {
        return with_options({}).scan(options);
    }

    scoped_operation<redis_scan_result> scan(redis_scan_options options = {}) const&& = delete;

    scoped_operation<redis_hash_scan_result> hscan(
        std::string_view key, redis_scan_options options = {}) const& {
        return with_options({}).hscan(key, options);
    }

    scoped_operation<redis_hash_scan_result> hscan(
        std::string_view key, redis_scan_options options = {}) const&& = delete;

    scoped_operation<redis_scan_result> sscan(
        std::string_view key, redis_scan_options options = {}) const& {
        return with_options({}).sscan(key, options);
    }

    scoped_operation<redis_scan_result> sscan(
        std::string_view key, redis_scan_options options = {}) const&& = delete;

    scoped_operation<redis_z_scan_result> zscan(
        std::string_view key, redis_scan_options options = {}) const& {
        return with_options({}).zscan(key, options);
    }

    scoped_operation<redis_z_scan_result> zscan(
        std::string_view key, redis_scan_options options = {}) const&& = delete;

    scoped_operation<redis_value> eval(std::string_view script,
        std::span<const std::string_view> keys = {},
        std::span<const std::string_view> args = {}) const& {
        return with_options({}).eval(script, keys, args);
    }

    scoped_operation<redis_value> eval(std::string_view script,
        std::span<const std::string_view> keys = {},
        std::span<const std::string_view> args = {}) const&& = delete;

    scoped_operation<redis_value> eval_sha(std::string_view sha1,
        std::span<const std::string_view> keys = {},
        std::span<const std::string_view> args = {}) const& {
        return with_options({}).eval_sha(sha1, keys, args);
    }

    scoped_operation<redis_value> eval_sha(std::string_view sha1,
        std::span<const std::string_view> keys = {},
        std::span<const std::string_view> args = {}) const&& = delete;

    scoped_operation<std::pmr::string> script_load(std::string_view script) const& {
        return with_options({}).script_load(script);
    }

    scoped_operation<std::pmr::string> script_load(std::string_view script) const&& = delete;

    scoped_operation<std::pmr::vector<bool>> script_exists(
        std::span<const std::string_view> sha1s) const& {
        return with_options({}).script_exists(sha1s);
    }

    scoped_operation<std::pmr::vector<bool>> script_exists(
        std::span<const std::string_view> sha1s) const&& = delete;

    scoped_operation<std::pmr::vector<bool>> script_exists(
        std::initializer_list<std::string_view> sha1s) const& = delete;

    scoped_operation<std::pmr::vector<bool>> script_exists(
        std::initializer_list<std::string_view> sha1s) const&& = delete;

    scoped_operation<std::optional<redis_key_value>> blpop(
        std::span<const std::string_view> keys, redis_block_wait wait) const& {
        return with_options({}).blpop(keys, wait);
    }

    scoped_operation<std::optional<redis_key_value>> blpop(
        std::span<const std::string_view> keys, redis_block_wait wait) const&& = delete;

    scoped_operation<std::optional<redis_key_value>> brpop(
        std::span<const std::string_view> keys, redis_block_wait wait) const& {
        return with_options({}).brpop(keys, wait);
    }

    scoped_operation<std::optional<redis_key_value>> brpop(
        std::span<const std::string_view> keys, redis_block_wait wait) const&& = delete;

    scoped_operation<std::optional<redis_x_read_group_result>> xread_group(std::string_view group,
        std::string_view consumer, std::span<const redis_stream_read_view> streams,
        redis_x_read_group_options options = {}) const& {
        return with_options({}).xread_group(group, consumer, streams, options);
    }

    scoped_operation<std::optional<redis_x_read_group_result>> xread_group(std::string_view group,
        std::string_view consumer, std::span<const redis_stream_read_view> streams,
        redis_x_read_group_options options = {}) const&& = delete;

    template <typename... args_type>
        requires detail::redis_argument_pack<args_type...>
    [[nodiscard]] scoped_operation<redis_value> command(args_type&&... args) const& {
        return with_options({}).command(std::forward<args_type>(args)...);
    }

    template <typename... args_type>
        requires detail::redis_argument_pack<args_type...>
    [[nodiscard]] scoped_operation<redis_value> command(args_type&&... args) const&& = delete;

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        keys_type&&... keys) const& {
        return with_options({}).mget(std::forward<keys_type>(keys)...);
    }

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        keys_type&&... keys) const&& = delete;

    template <typename... args_type>
        requires detail::redis_pair_argument_pack<args_type...>
    [[nodiscard]] scoped_operation<void> mset(args_type&&... args) const& {
        return with_options({}).mset(std::forward<args_type>(args)...);
    }

    template <typename... args_type>
        requires detail::redis_pair_argument_pack<args_type...>
    [[nodiscard]] scoped_operation<void> mset(args_type&&... args) const&& = delete;

    template <typename... args_type>
        requires(detail::redis_pair_argument_pack<args_type...> && sizeof...(args_type) >= 4)
    [[nodiscard]] scoped_operation<std::int64_t> hset(std::string_view key, args_type&&... args) const& {
        return with_options({}).hset(key, std::forward<args_type>(args)...);
    }

    template <typename... args_type>
        requires(detail::redis_pair_argument_pack<args_type...> && sizeof...(args_type) >= 4)
    [[nodiscard]] scoped_operation<std::int64_t> hset(std::string_view key, args_type&&... args) const&& = delete;

    template <typename... field_types>
        requires detail::redis_argument_pack<field_types...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, field_types&&... fields) const& {
        return with_options({}).hmget(key, std::forward<field_types>(fields)...);
    }

    template <typename... field_types>
        requires detail::redis_argument_pack<field_types...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, field_types&&... fields) const&& = delete;

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::pmr::string>> sinter(keys_type&&... keys) const& {
        return with_options({}).sinter(std::forward<keys_type>(keys)...);
    }

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::pmr::string>> sinter(keys_type&&... keys) const&& = delete;

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::pmr::string>> sunion(keys_type&&... keys) const& {
        return with_options({}).sunion(std::forward<keys_type>(keys)...);
    }

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::pmr::string>> sunion(keys_type&&... keys) const&& = delete;

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::pmr::string>> sdiff(keys_type&&... keys) const& {
        return with_options({}).sdiff(std::forward<keys_type>(keys)...);
    }

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::pmr::string>> sdiff(keys_type&&... keys) const&& = delete;

    template <typename... sha1s_type>
        requires detail::redis_argument_pack<sha1s_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<bool>> script_exists(sha1s_type&&... sha1s) const& {
        return with_options({}).script_exists(std::forward<sha1s_type>(sha1s)...);
    }

    template <typename... sha1s_type>
        requires detail::redis_argument_pack<sha1s_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<bool>> script_exists(sha1s_type&&... sha1s) const&& = delete;

    // Thread-safe close request; sockets are closed only on the bound loop.
    void close() noexcept;
    // Await on the bound loop to cancel and join all started operations.
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;
    [[nodiscard]] const worker_handle& worker() const& noexcept;
    const worker_handle& worker() const&& = delete;

private:
    std::shared_ptr<detail::redis_client_state> state_;
};

}  // namespace ruvia
