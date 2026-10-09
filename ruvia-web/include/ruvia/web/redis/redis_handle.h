#pragma once

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

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/detail/redis/redis_argument_pack.h"
#include "ruvia/web/detail/redis/redis_mapped_command.h"
#include "ruvia/web/redis/redis_pipeline.h"
#include "ruvia/web/redis/redis_repository_types.h"
#include "ruvia/web/redis/redis_transaction.h"

namespace ruvia {

namespace detail {
class redis_client_runtime;
}

template <typename entity_type>
class redis_repository;

class redis_handle final {
public:
    redis_handle(const redis_handle& other) noexcept;
    redis_handle& operator=(const redis_handle&) = delete;

    // Returns a request-scoped view whose policy is applied to every typed
    // command and inherited by pipelines and transactions created from it.
    [[nodiscard]] redis_handle with_options(operation_options options) const;

    // Borrows the owning worker; an expired capability cannot expose it.
    [[nodiscard]] const worker_handle& worker() const&;
    const worker_handle& worker() const&& = delete;

    scoped_operation<redis_value> command(std::span<const std::string_view> args) const;
    scoped_operation<redis_value> command(
        std::initializer_list<std::string_view> args) const = delete;

    scoped_operation<void> ping() const;
    scoped_operation<std::pmr::string> ping(std::string_view message) const;
    scoped_operation<std::optional<std::pmr::string>> get(std::string_view key) const;
    scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        std::span<const std::string_view> keys) const;
    scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        std::initializer_list<std::string_view> keys) const = delete;
    scoped_operation<redis_set_result> set(
        std::string_view key, std::string_view value, redis_set_options options = {}) const;
    scoped_operation<void> mset(
        std::span<const std::pair<std::string_view, std::string_view>> items) const;
    scoped_operation<void> mset(
        std::initializer_list<std::pair<std::string_view, std::string_view>> items) const = delete;
    scoped_operation<std::optional<std::pmr::string>> get_del(std::string_view key) const;
    scoped_operation<std::int64_t> append(std::string_view key, std::string_view value) const;
    scoped_operation<std::int64_t> strlen(std::string_view key) const;
    scoped_operation<std::int64_t> incr_by(std::string_view key, std::int64_t value) const;
    scoped_operation<std::int64_t> decr(std::string_view key) const;
    scoped_operation<std::int64_t> decr_by(std::string_view key, std::int64_t value) const;
    scoped_operation<std::int64_t> del(std::string_view key) const;
    scoped_operation<std::int64_t> unlink(std::string_view key) const;
    scoped_operation<bool> exists(std::string_view key) const;
    scoped_operation<bool> touch(std::string_view key) const;
    scoped_operation<std::pmr::string> type(std::string_view key) const;
    scoped_operation<void> rename(std::string_view key, std::string_view new_key) const;
    scoped_operation<bool> rename_nx(std::string_view key, std::string_view new_key) const;
    scoped_operation<bool> expire(std::string_view key, std::chrono::seconds ttl) const;
    scoped_operation<bool> expire_at(
        std::string_view key, std::chrono::system_clock::time_point expires_at) const;
    scoped_operation<bool> persist(std::string_view key) const;
    scoped_operation<redis_ttl> ttl(std::string_view key) const;
    scoped_operation<redis_ttl> pttl(std::string_view key) const;
    scoped_operation<std::int64_t> incr(std::string_view key) const;
    scoped_operation<std::optional<std::pmr::string>> hget(
        std::string_view key, std::string_view field) const;
    scoped_operation<std::int64_t> hset(
        std::string_view key, std::string_view field, std::string_view value) const;
    scoped_operation<std::int64_t> hset(std::string_view key,
        std::span<const std::pair<std::string_view, std::string_view>> fields) const;
    scoped_operation<std::int64_t> hset(std::string_view key,
        std::initializer_list<std::pair<std::string_view, std::string_view>> fields_value) const = delete;
    scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, std::span<const std::string_view> fields) const;
    scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, std::initializer_list<std::string_view> fields_value) const = delete;
    scoped_operation<std::pmr::vector<redis_key_value>> hget_all(std::string_view key) const;
    scoped_operation<std::int64_t> hdel(std::string_view key, std::string_view field) const;
    scoped_operation<bool> hexists(std::string_view key, std::string_view field) const;
    scoped_operation<std::int64_t> hlen(std::string_view key) const;
    scoped_operation<std::pmr::vector<std::pmr::string>> hkeys(std::string_view key) const;
    scoped_operation<std::pmr::vector<std::pmr::string>> hvals(std::string_view key) const;
    scoped_operation<std::int64_t> hincr_by(
        std::string_view key, std::string_view field, std::int64_t value) const;
    scoped_operation<std::int64_t> lpush(std::string_view key, std::string_view value) const;
    scoped_operation<std::int64_t> rpush(std::string_view key, std::string_view value) const;
    scoped_operation<std::optional<std::pmr::string>> lpop(std::string_view key) const;
    scoped_operation<std::optional<std::pmr::string>> rpop(std::string_view key) const;
    scoped_operation<std::int64_t> llen(std::string_view key) const;
    scoped_operation<std::pmr::vector<std::pmr::string>> lrange(
        std::string_view key, std::int64_t start, std::int64_t stop) const;
    scoped_operation<std::optional<std::pmr::string>> lindex(
        std::string_view key, std::int64_t index) const;
    scoped_operation<void> lset(
        std::string_view key, std::int64_t index, std::string_view value) const;
    scoped_operation<void> ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const;
    scoped_operation<std::int64_t> lrem(
        std::string_view key, std::int64_t count, std::string_view value) const;
    scoped_operation<std::int64_t> sadd(std::string_view key, std::string_view member) const;
    scoped_operation<std::int64_t> srem(std::string_view key, std::string_view member) const;
    scoped_operation<std::pmr::vector<std::pmr::string>> smembers(std::string_view key) const;
    scoped_operation<std::int64_t> scard(std::string_view key) const;
    scoped_operation<bool> sismember(std::string_view key, std::string_view member) const;
    scoped_operation<std::optional<std::pmr::string>> spop(std::string_view key) const;
    scoped_operation<std::optional<std::pmr::string>> srand_member(std::string_view key) const;
    scoped_operation<std::pmr::vector<std::pmr::string>> sinter(
        std::span<const std::string_view> keys) const;
    scoped_operation<std::pmr::vector<std::pmr::string>> sinter(
        std::initializer_list<std::string_view> keys) const = delete;
    scoped_operation<std::pmr::vector<std::pmr::string>> sunion(
        std::span<const std::string_view> keys) const;
    scoped_operation<std::pmr::vector<std::pmr::string>> sunion(
        std::initializer_list<std::string_view> keys) const = delete;
    scoped_operation<std::pmr::vector<std::pmr::string>> sdiff(
        std::span<const std::string_view> keys) const;
    scoped_operation<std::pmr::vector<std::pmr::string>> sdiff(
        std::initializer_list<std::string_view> keys) const = delete;
    scoped_operation<std::int64_t> zadd(
        std::string_view key, double score, std::string_view member) const;
    scoped_operation<std::int64_t> zrem(std::string_view key, std::string_view member) const;
    scoped_operation<std::pmr::vector<std::pmr::string>> zrange(
        std::string_view key, std::int64_t start, std::int64_t stop) const;
    scoped_operation<std::pmr::vector<redis_scored_value>> zrange_with_scores(
        std::string_view key, std::int64_t start, std::int64_t stop) const;
    scoped_operation<std::optional<double>> zscore(
        std::string_view key, std::string_view member) const;
    scoped_operation<std::int64_t> zcard(std::string_view key) const;
    scoped_operation<std::int64_t> zcount(std::string_view key, double min, double max) const;
    scoped_operation<redis_scan_result> scan(redis_scan_options options = {}) const;
    scoped_operation<redis_hash_scan_result> hscan(
        std::string_view key, redis_scan_options options = {}) const;
    scoped_operation<redis_scan_result> sscan(
        std::string_view key, redis_scan_options options = {}) const;
    scoped_operation<redis_z_scan_result> zscan(
        std::string_view key, redis_scan_options options = {}) const;
    scoped_operation<redis_value> eval(std::string_view script,
        std::span<const std::string_view> keys = {},
        std::span<const std::string_view> args = {}) const;
    scoped_operation<redis_value> eval_sha(std::string_view sha1,
        std::span<const std::string_view> keys = {},
        std::span<const std::string_view> args = {}) const;
    scoped_operation<std::pmr::string> script_load(std::string_view script) const;
    scoped_operation<std::pmr::vector<bool>> script_exists(
        std::span<const std::string_view> sha1s) const;
    scoped_operation<std::pmr::vector<bool>> script_exists(
        std::initializer_list<std::string_view> sha1s) const = delete;
    scoped_operation<std::optional<redis_key_value>> blpop(
        std::span<const std::string_view> keys, redis_block_wait wait) const;
    scoped_operation<std::optional<redis_key_value>> brpop(
        std::span<const std::string_view> keys, redis_block_wait wait) const;
    scoped_operation<std::optional<redis_x_read_group_result>> xread_group(std::string_view group,
        std::string_view consumer, std::span<const redis_stream_read_view> streams,
        redis_x_read_group_options options = {}) const;

    // Multi-argument commands as ordinary arguments: mget(a, b, c) instead of a
    // hand-built array plus a span. Every span overload above clones its
    // arguments into owned storage before returning -- the command is built
    // synchronously and only the owned copy is moved into the coroutine -- so the
    // temporary array each of these creates only has to outlive the call.
    //
    // A span argument is not convertible to string_view and therefore never
    // selects one of these; a caller that already holds a sequence keeps using
    // the span overload unchanged.
    template <typename... args_type>
        requires detail::redis_argument_pack<args_type...>
    [[nodiscard]] scoped_operation<redis_value> command(args_type&&... args) const {
        const std::string_view views[]{std::string_view(args)...};
        return command(std::span<const std::string_view>(views));
    }

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> mget(
        keys_type&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return mget(std::span<const std::string_view>(views));
    }

    // Alternating key/value arguments: mset(k1, v1, k2, v2).
    template <typename... args_type>
        requires detail::redis_pair_argument_pack<args_type...>
    [[nodiscard]] scoped_operation<void> mset(args_type&&... args) const {
        const auto items = pair_arguments(std::string_view(args)...);
        return mset(std::span<const std::pair<std::string_view, std::string_view>>(items));
    }

    // Four or more arguments; hset(key, field, value) stays on the single-field
    // overload above rather than routing one pair through the batch path.
    template <typename... args_type>
        requires(detail::redis_pair_argument_pack<args_type...> && sizeof...(args_type) >= 4)
    [[nodiscard]] scoped_operation<std::int64_t> hset(std::string_view key, args_type&&... args) const {
        const auto fields_value = pair_arguments(std::string_view(args)...);
        return hset(key, std::span<const std::pair<std::string_view, std::string_view>>(fields_value));
    }

    template <typename... field_types>
        requires detail::redis_argument_pack<field_types...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> hmget(
        std::string_view key, field_types&&... fields) const {
        const std::string_view views[]{std::string_view(fields)...};
        return hmget(key, std::span<const std::string_view>(views));
    }

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::pmr::string>> sinter(keys_type&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return sinter(std::span<const std::string_view>(views));
    }

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::pmr::string>> sunion(keys_type&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return sunion(std::span<const std::string_view>(views));
    }

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<std::pmr::string>> sdiff(keys_type&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return sdiff(std::span<const std::string_view>(views));
    }

    template <typename... sha1s_type>
        requires detail::redis_argument_pack<sha1s_type...>
    [[nodiscard]] scoped_operation<std::pmr::vector<bool>> script_exists(sha1s_type&&... sha1s) const {
        const std::string_view views[]{std::string_view(sha1s)...};
        return script_exists(std::span<const std::string_view>(views));
    }

    [[nodiscard]] redis_pipeline pipeline() const;
    [[nodiscard]] redis_transaction transaction() const;

    template <typename entity_type>
    [[nodiscard]] redis_repository<entity_type> get_repository(
        const redis_repository_config& config = {}) const;

private:
    // Reassembles a flat alternating argument list into the pair sequence the
    // span overloads take.
    template <typename... args_type>
    [[nodiscard]] static constexpr auto pair_arguments(args_type... args) {
        static_assert(sizeof...(args_type) % 2 == 0);
        const std::string_view flat[]{args...};
        std::array<std::pair<std::string_view, std::string_view>, sizeof...(args_type) / 2> pairs{};
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            pairs[index] = {flat[2 * index], flat[2 * index + 1]};
        }
        return pairs;
    }

    friend class detail::redis_client_runtime;
    template <typename entity_type>
    friend class redis_repository;

    [[nodiscard]] task<redis_value> command_owned(std::span<const std::string_view> args) const;

    template <typename result_type, typename mapper_type>
    [[nodiscard]] scoped_operation<result_type> command_mapped(std::span<const std::string_view> args, mapper_type mapper) const {
        registration_.require_active();
        return scoped(detail::map_redis_command<result_type>(command_owned(args), resource_, std::move(mapper)));
    }

    redis_handle(detail::redis_pool& general_pool, detail::redis_pool& blocking_pool_value,
        std::pmr::memory_resource* resource, operation_scope& operation_scope) noexcept;
    redis_handle(detail::redis_pool& general_pool, detail::redis_pool& blocking_pool_value,
        std::pmr::memory_resource* resource, operation_scope& operation_scope,
        operation_options options) noexcept;

    template <typename t_type>
    [[nodiscard]] scoped_operation<t_type> scoped(ruvia::task<t_type> task_value) const {
        return make_scoped_operation(registration_.scope(), std::move(task_value));
    }

    static void expire_capability(void* target) noexcept;
    [[nodiscard]] detail::redis_command_executor executor() const;
    [[nodiscard]] detail::redis_command_executor executor(detail::redis_pool& pool) const;
    detail::redis_pool* pool_;
    detail::redis_pool* blocking_pool_;
    std::pmr::memory_resource* resource_;
    operation_options operation_options_;
    scoped_capability_registration registration_;
};

}  // namespace ruvia
