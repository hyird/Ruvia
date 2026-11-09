#include <array>
#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_number_format.h"
#include "ruvia/web/detail/redis/redis_orm.h"
#include "ruvia/web/redis/redis.h"

#include "integration/request_io_metrics.h"
#include "redis/redis_client_runtime.h"
#include "redis/redis_command_args.h"
#include "redis/redis_command_routing.h"
#include "redis/redis_pool.h"
#include "redis/redis_reply_parsers.h"
#include "redis/redis_types_access.h"

namespace ruvia {
namespace {

template <typename duration_type>
void require_positive_redis_ttl(duration_type ttl, const char* message) {
    if (ttl.count() <= 0) {
        throw std::invalid_argument(message);
    }
}

[[nodiscard]] detail::redis_owned_command make_redis_scan_command(std::string_view command,
    std::optional<std::string_view> key, const redis_scan_options& options,
    std::pmr::memory_resource* resource) {
    std::array<char, http_max_unsigned_decimal_size> cursor_scratch{};
    std::array<char, http_max_unsigned_decimal_size> count_scratch{};
    std::array<std::string_view, 7> args{};
    std::size_t count = 0;
    args[count++] = command;
    if (key.has_value()) {
        args[count++] = *key;
    }
    args[count++] = http_format_unsigned(
        cursor_scratch, options.cursor_.has_value() ? detail::redis_types_access::cursor_value(*options.cursor_) : 0);
    if (options.match_.has_value()) {
        args[count++] = "MATCH";
        args[count++] = *options.match_;
    }
    if (options.count_.has_value()) {
        args[count++] = "COUNT";
        args[count++] = http_format_unsigned(count_scratch, options.count_->value());
    }
    return detail::make_owned_redis_command(
        resource, std::span<const std::string_view>(args.data(), count));
}

// The server-side wait starts when the command is written, so the reply
// deadline is armed at write time: slot acquisition and a lazy connect must not
// consume the block window. The grace covers the reply's network latency.
[[nodiscard]] std::optional<timeout_duration> redis_block_reply_timeout(
    const operation_options& options, redis_block_wait wait) {
    if (const auto duration = wait.duration(); duration.has_value()) {
        constexpr auto grace = std::chrono::seconds(1);
        if (*duration > std::chrono::milliseconds::max() - grace) {
            return std::chrono::milliseconds::max();
        }
        return *duration + grace;
    }
    detail::require_bounded_redis_wait(options, "infinite redis block requires a stop_token or finite operation timeout");
    return std::nullopt;
}

}  // namespace

redis_handle::redis_handle(detail::redis_client_runtime& runtime, std::pmr::memory_resource* resource,
    ::ruvia::operation_scope& operation_scope, detail::request_io_metrics* io_metrics) noexcept
    : runtime_(&runtime),
      resource_(pmr_resource_or_default(resource)),
      io_metrics_(io_metrics),
      registration_(operation_scope, this, &redis_handle::expire_capability) {}

redis_handle::redis_handle(detail::redis_client_runtime& runtime, std::pmr::memory_resource* resource,
    ::ruvia::operation_scope& operation_scope, operation_options options) noexcept
    : runtime_(&runtime),
      resource_(pmr_resource_or_default(resource)),
      operation_options_(std::move(options)),
      registration_(operation_scope, this, &redis_handle::expire_capability) {}

redis_handle::redis_handle(const redis_handle& other) noexcept
    : runtime_(other.runtime_),
      resource_(other.resource_),
      io_metrics_(other.io_metrics_),
      operation_options_(other.operation_options_),
      registration_(other.registration_, this) {}

redis_handle redis_handle::with_options(operation_options options) const {
    registration_.require_active();
    validate_operation_options(options);
    redis_handle configured(*this);
    configured.operation_options_ =
        override_operation_options(operation_options_, std::move(options));
    return configured;
}

const worker_handle& redis_handle::worker() const& {
    registration_.require_active();
    return runtime_->worker();
}

detail::redis_pool& redis_handle::pool() const noexcept {
    return runtime_->general();
}

detail::redis_pool& redis_handle::blocking_pool() const noexcept {
    return runtime_->blocking();
}

task<redis_value> redis_handle::execute(detail::redis_pool& pool_value, detail::redis_owned_command&& command,
    std::optional<timeout_duration> reply_timeout) const {
    return pool_value.execute_owned(std::move(command), resource_, operation_options_, reply_timeout, io_metrics_);
}

void redis_handle::expire_capability(void* target) noexcept {
    auto& handle = *static_cast<redis_handle*>(target);
    handle.runtime_ = nullptr;
    handle.resource_ = nullptr;
    handle.io_metrics_ = nullptr;
    handle.operation_options_ = {};
}

task<redis_value> redis_handle::command(std::span<const std::string_view> args) const {
    registration_.require_active();
    const bool blocking = detail::validate_redis_pooled_command(args, true);
    if (blocking) {
        detail::require_bounded_redis_wait(
            operation_options_, "raw blocking redis command requires a stop_token or finite operation timeout");
    }
    return scoped(execute(blocking ? blocking_pool() : pool(), detail::make_owned_redis_command(resource_, args)));
}

task<redis_value> redis_handle::command_owned(std::pmr::vector<std::pmr::string>&& args) const {
    registration_.require_active();
    if (args.get_allocator().resource() == resource_) {
        return pool().execute_owned(std::move(args), resource_, operation_options_, std::nullopt, io_metrics_);
    }
    return pool().execute_owned(std::pmr::vector<std::pmr::string>(args.begin(), args.end(), resource_), resource_,
        operation_options_, std::nullopt, io_metrics_);
}

task<redis_value> redis_handle::script_owned(
    const detail::redis_orm_script& script, std::pmr::vector<std::pmr::string>&& args) const {
    registration_.require_active();
    return detail::execute_redis_orm_script(
        pool(), script, std::move(args), io_metrics_, operation_options_, resource_);
}

task<void> redis_handle::ping() const {
    return mapped<void>({"PING"}, detail::parse_redis_pong);
}

task<std::pmr::string> redis_handle::ping(std::string_view message) const {
    return mapped<std::pmr::string>({"PING", message}, detail::take_redis_string);
}

task<std::optional<std::pmr::string>> redis_handle::get(std::string_view key) const {
    return mapped<std::optional<std::pmr::string>>({"GET", key}, detail::parse_redis_optional_string);
}

task<std::pmr::vector<std::optional<std::pmr::string>>> redis_handle::mget(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::optional<std::pmr::string>>>(
        pool(), detail::redis_command_with_keys("MGET", keys, resource_), detail::parse_redis_optional_string_array);
}

task<redis_set_result> redis_handle::set(
    std::string_view key, std::string_view value, redis_set_options options) const {
    registration_.require_active();
    auto command = detail::redis_set_args(key, value, options, resource_);
    return execute_mapped<redis_set_result>(pool(), std::move(command),
        [options = std::move(options)](redis_value&& reply, std::pmr::memory_resource* resource) {
            return detail::parse_redis_set_reply(std::move(reply), resource, options);
        });
}

task<void> redis_handle::mset(
    std::span<const std::pair<std::string_view, std::string_view>> items) const {
    registration_.require_active();
    return execute_mapped<void>(pool(), detail::redis_mset_args(items, resource_), detail::parse_redis_ok);
}

task<std::optional<std::pmr::string>> redis_handle::get_del(std::string_view key) const {
    return mapped<std::optional<std::pmr::string>>({"GETDEL", key}, detail::parse_redis_optional_string);
}

task<std::int64_t> redis_handle::append(
    std::string_view key, std::string_view value) const {
    return mapped<std::int64_t>({"APPEND", key, value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::strlen(std::string_view key) const {
    return mapped<std::int64_t>({"STRLEN", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::incr_by(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(
        pool(), detail::make_redis_integer_command("INCRBY", key, value, resource_), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::decr(std::string_view key) const {
    return mapped<std::int64_t>({"DECR", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::decr_by(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(
        pool(), detail::make_redis_integer_command("DECRBY", key, value, resource_), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::del(std::string_view key) const {
    return mapped<std::int64_t>({"DEL", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::del(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(
        pool(), detail::redis_command_with_keys("DEL", keys, resource_), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::unlink(std::string_view key) const {
    return mapped<std::int64_t>({"UNLINK", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::unlink(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(
        pool(), detail::redis_command_with_keys("UNLINK", keys, resource_), detail::parse_redis_integer);
}

task<bool> redis_handle::exists(std::string_view key) const {
    return mapped<bool>({"EXISTS", key}, detail::parse_redis_integer_bool);
}

task<bool> redis_handle::touch(std::string_view key) const {
    return mapped<bool>({"TOUCH", key}, detail::parse_redis_integer_bool);
}

task<std::pmr::string> redis_handle::type(std::string_view key) const {
    return mapped<std::pmr::string>({"TYPE", key}, detail::take_redis_string);
}

task<void> redis_handle::rename(std::string_view key, std::string_view new_key) const {
    return mapped<void>({"RENAME", key, new_key}, detail::parse_redis_ok);
}

task<bool> redis_handle::rename_nx(std::string_view key, std::string_view new_key) const {
    return mapped<bool>({"RENAMENX", key, new_key}, detail::parse_redis_integer_bool);
}

task<bool> redis_handle::expire(std::string_view key, std::chrono::seconds ttl) const {
    registration_.require_active();
    require_positive_redis_ttl(ttl, "redis expire TTL must be greater than zero");
    return execute_mapped<bool>(
        pool(), detail::make_redis_integer_command("EXPIRE", key, ttl.count(), resource_), detail::parse_redis_integer_bool);
}

task<bool> redis_handle::pexpire(std::string_view key, std::chrono::milliseconds ttl) const {
    registration_.require_active();
    require_positive_redis_ttl(ttl, "redis pexpire TTL must be greater than zero");
    return execute_mapped<bool>(
        pool(), detail::make_redis_integer_command("PEXPIRE", key, ttl.count(), resource_), detail::parse_redis_integer_bool);
}

task<bool> redis_handle::expire_at(
    std::string_view key, std::chrono::system_clock::time_point expires_at) const {
    registration_.require_active();
    const auto expires_ms =
        std::chrono::ceil<std::chrono::milliseconds>(expires_at.time_since_epoch()).count();
    return execute_mapped<bool>(
        pool(), detail::make_redis_integer_command("PEXPIREAT", key, expires_ms, resource_), detail::parse_redis_integer_bool);
}

task<bool> redis_handle::persist(std::string_view key) const {
    return mapped<bool>({"PERSIST", key}, detail::parse_redis_integer_bool);
}

task<redis_ttl> redis_handle::ttl(std::string_view key) const {
    return mapped<redis_ttl>({"TTL", key},
        [](redis_value&& reply, std::pmr::memory_resource*) { return detail::parse_redis_ttl(std::move(reply), true); });
}

task<redis_ttl> redis_handle::pttl(std::string_view key) const {
    return mapped<redis_ttl>({"PTTL", key},
        [](redis_value&& reply, std::pmr::memory_resource*) { return detail::parse_redis_ttl(std::move(reply), false); });
}

task<std::int64_t> redis_handle::incr(std::string_view key) const {
    return mapped<std::int64_t>({"INCR", key}, detail::parse_redis_integer);
}

task<std::optional<std::pmr::string>> redis_handle::hget(
    std::string_view key, std::string_view field) const {
    return mapped<std::optional<std::pmr::string>>({"HGET", key, field}, detail::parse_redis_optional_string);
}

task<std::int64_t> redis_handle::hset(
    std::string_view key, std::string_view field, std::string_view value) const {
    return mapped<std::int64_t>({"HSET", key, field, value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::hset(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields_value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(
        pool(), detail::redis_hset_fields_args(key, fields_value, resource_), detail::parse_redis_integer);
}

task<std::pmr::vector<std::optional<std::pmr::string>>> redis_handle::hmget(
    std::string_view key, std::span<const std::string_view> fields_value) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::optional<std::pmr::string>>>(
        pool(), detail::redis_command_with_key_fields("HMGET", key, fields_value, resource_),
        detail::parse_redis_optional_string_array);
}

task<std::pmr::vector<redis_key_value>> redis_handle::hget_all(std::string_view key) const {
    return mapped<std::pmr::vector<redis_key_value>>({"HGETALL", key},
        [](redis_value&& reply, std::pmr::memory_resource* resource) {
            return detail::parse_redis_key_value_array(std::move(reply), resource, "unexpected redis hgetall reply");
        });
}

task<std::int64_t> redis_handle::hdel(
    std::string_view key, std::string_view field) const {
    return mapped<std::int64_t>({"HDEL", key, field}, detail::parse_redis_integer);
}

task<bool> redis_handle::hexists(std::string_view key, std::string_view field) const {
    return mapped<bool>({"HEXISTS", key, field}, detail::parse_redis_integer_bool);
}

task<std::int64_t> redis_handle::hlen(std::string_view key) const {
    return mapped<std::int64_t>({"HLEN", key}, detail::parse_redis_integer);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::hkeys(std::string_view key) const {
    return mapped<std::pmr::vector<std::pmr::string>>({"HKEYS", key}, detail::parse_redis_string_array);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::hvals(std::string_view key) const {
    return mapped<std::pmr::vector<std::pmr::string>>({"HVALS", key}, detail::parse_redis_string_array);
}

task<std::int64_t> redis_handle::hincr_by(
    std::string_view key, std::string_view field, std::int64_t value) const {
    std::array<char, detail::redis_int_scratch_size> value_scratch{};
    return mapped<std::int64_t>(
        {"HINCRBY", key, field, detail::format_redis_int(value_scratch, value)}, detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::lpush(
    std::string_view key, std::string_view value) const {
    return mapped<std::int64_t>({"LPUSH", key, value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::rpush(
    std::string_view key, std::string_view value) const {
    return mapped<std::int64_t>({"RPUSH", key, value}, detail::parse_redis_integer);
}

task<std::optional<std::pmr::string>> redis_handle::lpop(std::string_view key) const {
    return mapped<std::optional<std::pmr::string>>({"LPOP", key}, detail::parse_redis_optional_string);
}

task<std::optional<std::pmr::string>> redis_handle::rpop(std::string_view key) const {
    return mapped<std::optional<std::pmr::string>>({"RPOP", key}, detail::parse_redis_optional_string);
}

task<std::int64_t> redis_handle::llen(std::string_view key) const {
    return mapped<std::int64_t>({"LLEN", key}, detail::parse_redis_integer);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::lrange(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(
        pool(), detail::make_redis_range_command("LRANGE", key, start, stop, resource_), detail::parse_redis_string_array);
}

task<std::optional<std::pmr::string>> redis_handle::lindex(
    std::string_view key, std::int64_t index) const {
    registration_.require_active();
    return execute_mapped<std::optional<std::pmr::string>>(
        pool(), detail::make_redis_integer_command("LINDEX", key, index, resource_), detail::parse_redis_optional_string);
}

task<void> redis_handle::lset(
    std::string_view key, std::int64_t index, std::string_view value) const {
    std::array<char, detail::redis_int_scratch_size> index_scratch{};
    return mapped<void>({"LSET", key, detail::format_redis_int(index_scratch, index), value}, detail::parse_redis_ok);
}

task<void> redis_handle::ltrim(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<void>(
        pool(), detail::make_redis_range_command("LTRIM", key, start, stop, resource_), detail::parse_redis_ok);
}

task<std::int64_t> redis_handle::lrem(
    std::string_view key, std::int64_t count, std::string_view value) const {
    std::array<char, detail::redis_int_scratch_size> count_scratch{};
    return mapped<std::int64_t>(
        {"LREM", key, detail::format_redis_int(count_scratch, count), value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::sadd(
    std::string_view key, std::string_view member) const {
    return mapped<std::int64_t>({"SADD", key, member}, detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::srem(
    std::string_view key, std::string_view member) const {
    return mapped<std::int64_t>({"SREM", key, member}, detail::parse_redis_integer);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::smembers(
    std::string_view key) const {
    return mapped<std::pmr::vector<std::pmr::string>>({"SMEMBERS", key}, detail::parse_redis_string_array);
}

task<std::int64_t> redis_handle::scard(std::string_view key) const {
    return mapped<std::int64_t>({"SCARD", key}, detail::parse_redis_integer);
}

task<bool> redis_handle::sismember(std::string_view key, std::string_view member) const {
    return mapped<bool>({"SISMEMBER", key, member}, detail::parse_redis_integer_bool);
}

task<std::optional<std::pmr::string>> redis_handle::spop(std::string_view key) const {
    return mapped<std::optional<std::pmr::string>>({"SPOP", key}, detail::parse_redis_optional_string);
}

task<std::optional<std::pmr::string>> redis_handle::srand_member(
    std::string_view key) const {
    return mapped<std::optional<std::pmr::string>>({"SRANDMEMBER", key}, detail::parse_redis_optional_string);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::sinter(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(
        pool(), detail::redis_command_with_keys("SINTER", keys, resource_), detail::parse_redis_string_array);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::sunion(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(
        pool(), detail::redis_command_with_keys("SUNION", keys, resource_), detail::parse_redis_string_array);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::sdiff(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(
        pool(), detail::redis_command_with_keys("SDIFF", keys, resource_), detail::parse_redis_string_array);
}

task<std::int64_t> redis_handle::zadd(
    std::string_view key, double score, std::string_view member) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(
        pool(), detail::make_redis_zadd_command(key, score, member, resource_), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::zrem(
    std::string_view key, std::string_view member) const {
    return mapped<std::int64_t>({"ZREM", key, member}, detail::parse_redis_integer);
}

task<std::pmr::vector<std::pmr::string>> redis_handle::zrange(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::pmr::string>>(
        pool(), detail::make_redis_range_command("ZRANGE", key, start, stop, resource_), detail::parse_redis_string_array);
}

task<std::pmr::vector<redis_scored_value>> redis_handle::zrange_with_scores(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    auto command = detail::make_redis_range_command("ZRANGE", key, start, stop, resource_, true);
    return execute_mapped<std::pmr::vector<redis_scored_value>>(pool(), std::move(command), detail::parse_redis_scored_array);
}

task<std::optional<double>> redis_handle::zscore(
    std::string_view key, std::string_view member) const {
    return mapped<std::optional<double>>({"ZSCORE", key, member},
        [](redis_value&& reply, std::pmr::memory_resource*) {
            return detail::parse_redis_optional_double(std::move(reply), "invalid redis zscore reply");
        });
}

task<std::int64_t> redis_handle::zcard(std::string_view key) const {
    return mapped<std::int64_t>({"ZCARD", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::zcount(
    std::string_view key, double min_value, double max_value) const {
    registration_.require_active();
    std::array<char, detail::redis_score_scratch_size> min_scratch{};
    std::array<char, detail::redis_score_scratch_size> max_scratch{};
    const std::string_view args[]{
        "ZCOUNT",
        key,
        detail::format_redis_score(min_scratch, min_value),
        detail::format_redis_score(max_scratch, max_value),
    };
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, args), detail::parse_redis_integer);
}

task<redis_scan_result> redis_handle::scan(redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_scan_result>(
        pool(), make_redis_scan_command("SCAN", std::nullopt, options, resource_), detail::parse_redis_scan_result);
}

task<redis_hash_scan_result> redis_handle::hscan(
    std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_hash_scan_result>(
        pool(), make_redis_scan_command("HSCAN", key, options, resource_), detail::parse_redis_hash_scan_result);
}

task<redis_scan_result> redis_handle::sscan(
    std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_scan_result>(
        pool(), make_redis_scan_command("SSCAN", key, options, resource_), detail::parse_redis_scan_result);
}

task<redis_z_scan_result> redis_handle::zscan(
    std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_z_scan_result>(
        pool(), make_redis_scan_command("ZSCAN", key, options, resource_), detail::parse_redis_z_scan_result);
}

task<redis_value> redis_handle::eval(std::string_view script,
    std::span<const std::string_view> keys, std::span<const std::string_view> args) const {
    registration_.require_active();
    return scoped(execute(pool(), detail::redis_eval_args("EVAL", script, keys, args, resource_)));
}

task<redis_value> redis_handle::eval_sha(std::string_view sha1,
    std::span<const std::string_view> keys, std::span<const std::string_view> args) const {
    registration_.require_active();
    return scoped(execute(pool(), detail::redis_eval_args("EVALSHA", sha1, keys, args, resource_)));
}

task<std::pmr::string> redis_handle::script_load(std::string_view script) const {
    return mapped<std::pmr::string>({"SCRIPT", "LOAD", script}, detail::take_redis_string);
}

task<std::pmr::vector<bool>> redis_handle::script_exists(
    std::span<const std::string_view> sha1s) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<bool>>(
        pool(), detail::redis_command_with_key_fields("SCRIPT", "EXISTS", sha1s, resource_), detail::parse_redis_bool_array);
}

task<std::optional<redis_key_value>> redis_handle::blpop(
    std::span<const std::string_view> keys, redis_block_wait wait) const {
    registration_.require_active();
    const auto reply_timeout = redis_block_reply_timeout(operation_options_, wait);
    return execute_mapped<std::optional<redis_key_value>>(blocking_pool(),
        detail::redis_blocking_pop_args("BLPOP", keys, wait, resource_), detail::parse_redis_blocking_pop_reply,
        reply_timeout);
}

task<std::optional<redis_key_value>> redis_handle::brpop(
    std::span<const std::string_view> keys, redis_block_wait wait) const {
    registration_.require_active();
    const auto reply_timeout = redis_block_reply_timeout(operation_options_, wait);
    return execute_mapped<std::optional<redis_key_value>>(blocking_pool(),
        detail::redis_blocking_pop_args("BRPOP", keys, wait, resource_), detail::parse_redis_blocking_pop_reply,
        reply_timeout);
}

task<std::optional<redis_x_read_group_result>> redis_handle::xread_group(
    std::string_view group, std::string_view consumer, std::span<const redis_stream_read_view> streams,
    redis_x_read_group_options options) const {
    registration_.require_active();
    auto args = detail::redis_x_read_group_args(group, consumer, streams, options, resource_);
    auto* selected_pool = &pool();
    std::optional<timeout_duration> reply_timeout;
    if (options.block_.has_value()) {
        selected_pool = &blocking_pool();
        reply_timeout = redis_block_reply_timeout(operation_options_, *options.block_);
    }
    return execute_mapped<std::optional<redis_x_read_group_result>>(
        *selected_pool, std::move(args), detail::parse_redis_x_read_group_reply, reply_timeout);
}

redis_pipeline redis_handle::pipeline() const {
    registration_.require_active();
    return redis_pipeline(pool(), operation_options_, resource_, registration_.scope(), io_metrics_);
}

redis_transaction redis_handle::transaction() const {
    registration_.require_active();
    return redis_transaction(pool(), operation_options_, resource_, registration_.scope(), io_metrics_);
}

task<std::int64_t> redis_handle::publish(std::string_view channel, std::string_view payload) const {
    return mapped<std::int64_t>({"PUBLISH", channel, payload}, detail::parse_redis_integer);
}

redis_subscription redis_handle::subscription() const {
    registration_.require_active();
    detail::require_bounded_redis_wait(
        operation_options_, "redis subscription requires a stop_token or finite operation timeout");
    return redis_subscription(runtime_->subscriptions(), registration_.scope(), operation_options_);
}

namespace detail {

template <redis_batch_mode mode>
redis_batch<mode>::redis_batch(redis_pool& pool, operation_options options, std::pmr::memory_resource* resource,
    operation_scope& scope, request_io_metrics* io_metrics) noexcept
    : pool_(&pool),
      options_(std::move(options)),
      commands_(pmr_resource_or_default(resource)),
      io_metrics_(io_metrics),
      registration_(scope, this, &redis_batch::expire_capability) {}

template <redis_batch_mode mode>
redis_batch<mode>::redis_batch(redis_batch&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)),
      options_(std::move(other.options_)),
      commands_(std::move(other.commands_)),
      io_metrics_(other.io_metrics_),
      registration_(std::move(other.registration_), this) {}

template <redis_batch_mode mode>
void redis_batch<mode>::require_ready() const {
    registration_.require_active();
    if (pool_ == nullptr) {
        throw std::logic_error("redis command batch has already been consumed or expired");
    }
}

template <redis_batch_mode mode>
std::pmr::memory_resource* redis_batch<mode>::resource() const noexcept {
    return commands_.get_allocator().resource();
}

template <redis_batch_mode mode>
redis_command_payload redis_batch<mode>::consume() {
    require_ready();
    auto& pool = *std::exchange(pool_, nullptr);
    return {pool, std::move(options_), std::move(commands_)};
}

template <redis_batch_mode mode>
void redis_batch<mode>::expire() noexcept {
    pool_ = nullptr;
    options_ = {};
    io_metrics_ = nullptr;
    std::pmr::vector<redis_owned_command> empty(resource());
    commands_.swap(empty);
}

template <redis_batch_mode mode>
void redis_batch<mode>::expire_capability(void* target) noexcept {
    static_cast<redis_batch*>(target)->expire();
}

template <redis_batch_mode mode>
redis_batch<mode>& redis_batch<mode>::command(std::span<const std::string_view> args) {
    require_ready();
    const bool barrier = validate_redis_pooled_command(args, false, mode == redis_batch_mode::pipeline);
    if (barrier) {
        require_bounded_redis_wait(options_, "redis barrier pipeline requires a stop_token or finite operation timeout");
    }
    return append_owned(make_owned_redis_command(resource(), args));
}

template <redis_batch_mode mode>
redis_batch<mode>& redis_batch<mode>::append_owned(redis_owned_command command) {
    commands_.push_back(std::move(command));
    return *this;
}

template <redis_batch_mode mode>
task<std::pmr::vector<redis_value>> redis_batch<mode>::execute_owned(
    request_io_metrics* io_metrics, redis_command_payload payload_value) {
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    auto* resource = payload_value.commands_.get_allocator().resource();
    if constexpr (mode == redis_batch_mode::pipeline) {
        co_return co_await payload_value.pool_.get().execute_pipeline(
            std::move(payload_value.commands_), std::move(payload_value.options_), resource);
    } else {
        auto replies = co_await payload_value.pool_.get().execute_pipeline(
            std::move(payload_value.commands_), std::move(payload_value.options_), resource, true);
        if (replies.empty()) {
            throw redis_error(redis_error::code_type::protocol_error, "redis transaction returned no replies");
        }
        for (std::size_t i = 0; i < replies.size(); ++i) {
            throw_if_redis_transaction_reply_error(replies[i], i);
        }
        auto exec_reply = std::move(replies.back());
        if (exec_reply.kind() != redis_value::kind_type::array) {
            throw redis_error(redis_error::code_type::command_error, "unexpected redis transaction reply");
        }

        // Every reply node lives in `resource`, so the EXEC elements move without copying.
        co_return std::move(redis_types_access::elements(exec_reply));
    }
}

template <redis_batch_mode mode>
task<std::pmr::vector<redis_value>> redis_batch<mode>::exec() && {
    auto& scope = registration_.scope();
    auto payload_value = consume();
    return make_scoped_operation(scope, execute_owned(io_metrics_, std::move(payload_value)));
}

template class redis_batch<redis_batch_mode::pipeline>;
template class redis_batch<redis_batch_mode::transaction>;

}  // namespace detail
}  // namespace ruvia
