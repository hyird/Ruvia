#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/detail/redis/redis_argument_pack.h"
#include "ruvia/web/detail/redis/redis_mapped_command.h"
#include "ruvia/web/detail/redis/redis_owned_command.h"
#include "ruvia/web/redis/redis_types.h"

namespace ruvia {

class redis_handle;

namespace detail {
class redis_client_runtime;
class redis_client_state;
struct redis_command_executor;
class redis_pool;
struct redis_orm_script;
class redis_subscription_connection;
class redis_subscription_set;
struct redis_types_access;

struct redis_command_payload final {
    std::reference_wrapper<redis_pool> pool_;
    operation_options options_;
    std::pmr::vector<redis_owned_command> commands_;
};

enum class redis_batch_mode : std::uint8_t {
    pipeline,
    transaction,
};

template <redis_batch_mode mode>
class redis_batch final {
public:
    redis_batch(const redis_batch&) = delete;
    redis_batch& operator=(const redis_batch&) = delete;
    redis_batch(redis_batch&& other) noexcept;
    redis_batch& operator=(redis_batch&&) = delete;

    redis_batch& command(std::span<const std::string_view> args);
    redis_batch& command(std::initializer_list<std::string_view> args) = delete;

    // Every command owns its arguments before this call returns.
    template <typename... args_type>
        requires redis_argument_pack<args_type...>
    redis_batch& command(args_type&&... args) {
        const std::string_view views[]{std::string_view(args)...};
        return command(std::span<const std::string_view>(views));
    }

    redis_batch& get(std::string_view key) {
        return trusted("GET", key);
    }

    redis_batch& set(std::string_view key, std::string_view value) {
        return trusted("SET", key, value);
    }

    redis_batch& get_del(std::string_view key) {
        return trusted("GETDEL", key);
    }

    redis_batch& append(std::string_view key, std::string_view value) {
        return trusted("APPEND", key, value);
    }

    redis_batch& strlen(std::string_view key) {
        return trusted("STRLEN", key);
    }

    redis_batch& del(std::string_view key) {
        return trusted("DEL", key);
    }

    redis_batch& unlink(std::string_view key) {
        return trusted("UNLINK", key);
    }

    redis_batch& exists(std::string_view key) {
        return trusted("EXISTS", key);
    }

    redis_batch& touch(std::string_view key) {
        return trusted("TOUCH", key);
    }

    redis_batch& type(std::string_view key) {
        return trusted("TYPE", key);
    }

    redis_batch& rename(std::string_view key, std::string_view new_key) {
        return trusted("RENAME", key, new_key);
    }

    redis_batch& rename_nx(std::string_view key, std::string_view new_key) {
        return trusted("RENAMENX", key, new_key);
    }

    redis_batch& incr(std::string_view key) {
        return trusted("INCR", key);
    }

    redis_batch& incr_by(std::string_view key, std::int64_t value) {
        require_ready();
        return append_owned(make_redis_integer_command("INCRBY", key, value, resource()));
    }

    redis_batch& decr(std::string_view key) {
        return trusted("DECR", key);
    }

    redis_batch& decr_by(std::string_view key, std::int64_t value) {
        require_ready();
        return append_owned(make_redis_integer_command("DECRBY", key, value, resource()));
    }

    redis_batch& hget(std::string_view key, std::string_view field) {
        return trusted("HGET", key, field);
    }

    redis_batch& hset(std::string_view key, std::string_view field, std::string_view value) {
        return trusted("HSET", key, field, value);
    }

    redis_batch& hdel(std::string_view key, std::string_view field) {
        return trusted("HDEL", key, field);
    }

    redis_batch& hexists(std::string_view key, std::string_view field) {
        return trusted("HEXISTS", key, field);
    }

    redis_batch& hlen(std::string_view key) {
        return trusted("HLEN", key);
    }

    redis_batch& hget_all(std::string_view key) {
        return trusted("HGETALL", key);
    }

    redis_batch& lpush(std::string_view key, std::string_view value) {
        return trusted("LPUSH", key, value);
    }

    redis_batch& rpush(std::string_view key, std::string_view value) {
        return trusted("RPUSH", key, value);
    }

    redis_batch& lpop(std::string_view key) {
        return trusted("LPOP", key);
    }

    redis_batch& rpop(std::string_view key) {
        return trusted("RPOP", key);
    }

    redis_batch& llen(std::string_view key) {
        return trusted("LLEN", key);
    }

    redis_batch& lrange(std::string_view key, std::int64_t start, std::int64_t stop) {
        require_ready();
        return append_owned(make_redis_range_command("LRANGE", key, start, stop, resource()));
    }

    redis_batch& sadd(std::string_view key, std::string_view member) {
        return trusted("SADD", key, member);
    }

    redis_batch& srem(std::string_view key, std::string_view member) {
        return trusted("SREM", key, member);
    }

    redis_batch& smembers(std::string_view key) {
        return trusted("SMEMBERS", key);
    }

    redis_batch& scard(std::string_view key) {
        return trusted("SCARD", key);
    }

    redis_batch& zadd(std::string_view key, double score, std::string_view member) {
        require_ready();
        return append_owned(make_redis_zadd_command(key, score, member, resource()));
    }

    redis_batch& zrem(std::string_view key, std::string_view member) {
        return trusted("ZREM", key, member);
    }

    redis_batch& zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
        require_ready();
        return append_owned(make_redis_range_command("ZRANGE", key, start, stop, resource()));
    }

    redis_batch& zscore(std::string_view key, std::string_view member) {
        return trusted("ZSCORE", key, member);
    }

    redis_batch& zcard(std::string_view key) {
        return trusted("ZCARD", key);
    }

    redis_batch& publish(std::string_view channel, std::string_view payload) {
        return trusted("PUBLISH", channel, payload);
    }

    // Consumes the batch before returning the lazy task, so the coroutine frame
    // owns every command and never borrows this builder through `this`.
    scoped_operation<std::pmr::vector<redis_value>> exec() &&;

private:
    friend class ::ruvia::redis_handle;

    redis_batch(redis_pool& pool, operation_options options,
        std::pmr::memory_resource* resource, operation_scope& scope) noexcept;
    void require_ready() const;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] redis_command_payload consume();
    void expire() noexcept;
    redis_batch& append_owned(redis_owned_command command);

    template <typename... args_type>
    redis_batch& trusted(std::string_view command_name, args_type... args) {
        require_ready();
        const std::string_view rest[]{args...};
        return append_owned(make_owned_redis_command(resource(), command_name, std::span<const std::string_view>(rest)));
    }

    [[nodiscard]] static task<std::pmr::vector<redis_value>> execute_owned(
        operation_scope& scope, redis_command_payload payload);
    static void expire_capability(void* target) noexcept;

    redis_pool* pool_;
    operation_options options_;
    std::pmr::vector<redis_owned_command> commands_;
    scoped_capability_registration registration_;
};

}  // namespace detail

using redis_pipeline = detail::redis_batch<detail::redis_batch_mode::pipeline>;
using redis_transaction = detail::redis_batch<detail::redis_batch_mode::transaction>;

enum class redis_pubsub_event_kind : std::uint8_t {
    // A SUBSCRIBE or PSUBSCRIBE confirmation for one channel or pattern.
    subscribed,
    // An UNSUBSCRIBE or PUNSUBSCRIBE confirmation for one channel or pattern.
    unsubscribed,
    // A published message. pattern() is set when a pattern subscription
    // delivered it.
    message,
};

// One Pub/Sub reply in wire order. Every view borrows the subscription's
// current reply storage and is valid until the next read() starts or the
// subscription is destroyed; copy what outlives that.
class redis_pubsub_event final {
public:
    [[nodiscard]] redis_pubsub_event_kind kind() const noexcept {
        return kind_;
    }

    // The channel a message was published to, or the channel a confirmation
    // names; empty for a pattern confirmation.
    [[nodiscard]] std::string_view channel() const noexcept {
        return channel_;
    }

    // The matched pattern of a pattern subscription; empty otherwise.
    [[nodiscard]] std::string_view pattern() const noexcept {
        return pattern_;
    }

    [[nodiscard]] std::string_view payload() const noexcept {
        return payload_;
    }

    // Channels and patterns this connection is subscribed to after a
    // confirmation; zero for a message.
    [[nodiscard]] std::int64_t subscription_count() const noexcept {
        return subscription_count_;
    }

private:
    friend struct detail::redis_types_access;

    redis_pubsub_event(redis_pubsub_event_kind kind, std::string_view channel, std::string_view pattern,
        std::string_view payload, std::int64_t subscription_count) noexcept
        : kind_(kind),
          channel_(channel),
          pattern_(pattern),
          payload_(payload),
          subscription_count_(subscription_count) {}

    redis_pubsub_event_kind kind_;
    std::string_view channel_;
    std::string_view pattern_;
    std::string_view payload_;
    std::int64_t subscription_count_;
};

// One dedicated Pub/Sub connection, created by redis_handle::subscription().
// Channels and patterns are multiplexed on it; ordinary commands stay on the
// handle's pools. The connection is opened by the first subscribe() and lives
// until abort(), the handle's stop token, worker shutdown, a failed operation
// or destruction. There is no reconnect: Pub/Sub delivers at most once, and a
// closed subscription has lost whatever was published meanwhile, so the
// application decides how to recover.
//
// Commands only write; their confirmations arrive through read() in wire order,
// interleaved with messages. One command and one read may be in flight at a
// time (a second throws std::logic_error); unstarted operations may coexist.
// Reads observe the subscription's stop token and operation timeout; a timeout
// closes the subscription and throws redis_error(timeout).
//
// Worker-affine: use and destroy it on the owning worker before the handle's
// scope ends. Destroying it with an operation in flight terminates.
class redis_subscription final {
public:
    redis_subscription(const redis_subscription&) = delete;
    redis_subscription& operator=(const redis_subscription&) = delete;
    redis_subscription(redis_subscription&&) = delete;
    redis_subscription& operator=(redis_subscription&&) = delete;
    ~redis_subscription();

    // Non-empty name lists; names are binary-safe.
    scoped_operation<void> subscribe(std::span<const std::string_view> channels) &;
    scoped_operation<void> subscribe(std::span<const std::string_view>) && = delete;
    scoped_operation<void> psubscribe(std::span<const std::string_view> patterns) &;
    scoped_operation<void> psubscribe(std::span<const std::string_view>) && = delete;
    // An empty list unsubscribes from every channel (pattern) of this connection.
    scoped_operation<void> unsubscribe(std::span<const std::string_view> channels = {}) &;
    scoped_operation<void> unsubscribe(std::span<const std::string_view> = {}) && = delete;
    scoped_operation<void> punsubscribe(std::span<const std::string_view> patterns = {}) &;
    scoped_operation<void> punsubscribe(std::span<const std::string_view> = {}) && = delete;

    template <typename... channels_type>
        requires detail::redis_argument_pack<channels_type...>
    [[nodiscard]] scoped_operation<void> subscribe(channels_type&&... channels) & {
        const std::string_view views[]{std::string_view(channels)...};
        return subscribe(std::span<const std::string_view>(views));
    }
    template <typename... channels_type>
        requires detail::redis_argument_pack<channels_type...>
    scoped_operation<void> subscribe(channels_type&&...) && = delete;

    template <typename... patterns_type>
        requires detail::redis_argument_pack<patterns_type...>
    [[nodiscard]] scoped_operation<void> psubscribe(patterns_type&&... patterns) & {
        const std::string_view views[]{std::string_view(patterns)...};
        return psubscribe(std::span<const std::string_view>(views));
    }
    template <typename... patterns_type>
        requires detail::redis_argument_pack<patterns_type...>
    scoped_operation<void> psubscribe(patterns_type&&...) && = delete;

    template <typename... channels_type>
        requires detail::redis_argument_pack<channels_type...>
    [[nodiscard]] scoped_operation<void> unsubscribe(channels_type&&... channels) & {
        const std::string_view views[]{std::string_view(channels)...};
        return unsubscribe(std::span<const std::string_view>(views));
    }
    template <typename... channels_type>
        requires detail::redis_argument_pack<channels_type...>
    scoped_operation<void> unsubscribe(channels_type&&...) && = delete;

    template <typename... patterns_type>
        requires detail::redis_argument_pack<patterns_type...>
    [[nodiscard]] scoped_operation<void> punsubscribe(patterns_type&&... patterns) & {
        const std::string_view views[]{std::string_view(patterns)...};
        return punsubscribe(std::span<const std::string_view>(views));
    }
    template <typename... patterns_type>
        requires detail::redis_argument_pack<patterns_type...>
    scoped_operation<void> punsubscribe(patterns_type&&...) && = delete;
    // The next confirmation or message. std::nullopt once the subscription is
    // closed by abort(), its stop token or worker shutdown. The server ending
    // the connection throws redis_error(io_error); an error reply (for example
    // a denied channel) throws redis_error(command_error) and leaves the
    // connection open; a malformed push closes it with protocol_error.
    // Requires a completed subscribe() first: std::logic_error otherwise.
    [[nodiscard]] scoped_operation<std::optional<redis_pubsub_event>> read() &;
    scoped_operation<std::optional<redis_pubsub_event>> read() && = delete;

    // Closes the connection now; a pending read() completes with std::nullopt.
    void abort() noexcept;

private:
    friend class redis_handle;

    redis_subscription(detail::redis_subscription_set& subscriptions, operation_scope& scope,
        operation_options options);

    static void expire_capability(void* target) noexcept;
    scoped_operation<void> send(std::string_view command, std::span<const std::string_view> names, bool require_names);

    std::unique_ptr<detail::redis_subscription_connection, pmr_object_deleter<detail::redis_subscription_connection>>
        connection_;
    // Declared after the connection so it unlinks before the connection closes.
    scoped_capability_registration registration_;
};

struct redis_repository_config;
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
    [[nodiscard]] scoped_operation<std::int64_t> del(std::span<const std::string_view> keys) const;
    scoped_operation<std::int64_t> unlink(std::string_view key) const;
    [[nodiscard]] scoped_operation<std::int64_t> unlink(std::span<const std::string_view> keys) const;
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

    // Delivers to a channel on the ordinary pool; returns the receiver count.
    scoped_operation<std::int64_t> publish(std::string_view channel, std::string_view payload) const;
    // A dedicated Pub/Sub connection bound to this handle's scope, stop token
    // and operation timeout. Like an infinite blocking wait, it requires a
    // stoppable token or a finite timeout (std::invalid_argument otherwise).
    [[nodiscard]] redis_subscription subscription() const;

    // Validates `config` against the entity schema: the prefix must be
    // non-empty without NUL bytes, and each index column must be a unique,
    // identifier-named entity column whose type fits the index kind.
    // Violations throw std::invalid_argument.
    template <typename entity_type>
    [[nodiscard]] redis_repository<entity_type> repository() const;
    template <typename entity_type>
    [[nodiscard]] redis_repository<entity_type> repository(
        const redis_repository_config& config) const;

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

    // ORM commands are compiled by the framework straight into owned storage
    // and are never stateful or blocking. The storage is moved into the
    // command when it already lives in the handle's resource and copied into
    // that resource otherwise.
    [[nodiscard]] task<redis_value> command_owned(std::pmr::vector<std::pmr::string>&& args) const;
    // ORM scripts run as EVALSHA; a NOSCRIPT reply loads the body through
    // EVAL within the same operation.
    [[nodiscard]] task<redis_value> script_owned(
        const detail::redis_orm_script& script, std::pmr::vector<std::pmr::string>&& args) const;

    template <typename result_type, typename mapper_type>
    [[nodiscard]] scoped_operation<result_type> command_mapped(
        std::pmr::vector<std::pmr::string>&& args, mapper_type mapper) const {
        registration_.require_active();
        return scoped(detail::map_redis_command<result_type>(command_owned(std::move(args)), resource_, std::move(mapper)));
    }

    template <typename result_type, typename mapper_type>
    [[nodiscard]] scoped_operation<result_type> script_mapped(const detail::redis_orm_script& script,
        std::pmr::vector<std::pmr::string>&& args, mapper_type mapper) const {
        registration_.require_active();
        return scoped(detail::map_redis_command<result_type>(
            script_owned(script, std::move(args)), resource_, std::move(mapper)));
    }

    redis_handle(detail::redis_client_runtime& runtime, std::pmr::memory_resource* resource,
        operation_scope& operation_scope) noexcept;
    redis_handle(detail::redis_client_runtime& runtime, std::pmr::memory_resource* resource,
        operation_scope& operation_scope, operation_options options) noexcept;

    template <typename t_type>
    [[nodiscard]] scoped_operation<t_type> scoped(ruvia::task<t_type> task_value) const {
        return make_scoped_operation(registration_.scope(), std::move(task_value));
    }

    static void expire_capability(void* target) noexcept;
    [[nodiscard]] detail::redis_command_executor executor() const;
    [[nodiscard]] detail::redis_command_executor executor(detail::redis_pool& pool) const;
    [[nodiscard]] detail::redis_pool& pool() const noexcept;
    [[nodiscard]] detail::redis_pool& blocking_pool() const noexcept;
    detail::redis_client_runtime* runtime_;
    std::pmr::memory_resource* resource_;
    operation_options operation_options_;
    scoped_capability_registration registration_;
};

#ifdef RUVIA_ENABLE_REDIS
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
    // Returns a handle with the supplied options. Each call registers one allocation-free
    // capability with the client operation scope, so keep the returned handle for
    // a sequence of commands instead of calling handle() per command.
    [[nodiscard]] redis_handle handle(operation_options options = {}) const&;
    redis_handle handle(operation_options = {}) const&& = delete;

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
#endif

}  // namespace ruvia
