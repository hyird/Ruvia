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
#include "ruvia/web/redis/redis.h"

#include "redis/redis_client_runtime.h"
#include "redis/redis_command_args.h"
#include "redis/redis_command_routing.h"
#include "redis/redis_pool.h"
#include "redis/redis_reply_parsers.h"
#include "redis/redis_types_access.h"

namespace ruvia {
namespace {

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
[[nodiscard]] std::optional<std::chrono::milliseconds> redis_block_reply_timeout(
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
    registration_.require_active();
    return execute_mapped<std::pmr::string>(pool(), detail::make_owned_redis_command(resource_, {"SCRIPT", "LOAD", script}), detail::take_redis_string);
}

task<std::pmr::vector<bool>> redis_handle::script_exists(
    std::span<const std::string_view> sha1s) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<bool>>(pool(), detail::redis_command_with_key_fields("SCRIPT", "EXISTS", sha1s, resource_), detail::parse_redis_bool_array);
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
    std::optional<std::chrono::milliseconds> reply_timeout;
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
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"PUBLISH", channel, payload}), detail::parse_redis_integer);
}

redis_subscription redis_handle::subscription() const {
    registration_.require_active();
    detail::require_bounded_redis_wait(
        operation_options_, "redis subscription requires a stop_token or finite operation timeout");
    return redis_subscription(runtime_->subscriptions(), registration_.scope(), operation_options_);
}

}  // namespace ruvia
