#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/detail/redis/redis_utils.h"
#include "ruvia/web/redis/redis.h"

#include "redis/redis_handle_helpers.h"
#include "redis/redis_registry.h"

namespace ruvia {
namespace {

[[nodiscard]] std::pmr::vector<std::pmr::string> redis_scan_args(std::string_view command,
    const redis_scan_options& options, std::pmr::memory_resource* resource) {
    auto cursor_value = detail::redis_cursor_string(options.cursor_, resource);
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(6);
    detail::emplace_redis_string(args, command);
    args.emplace_back(std::move(cursor_value));
    detail::append_redis_scan_options(args, options, resource);
    return args;
}

[[nodiscard]] std::pmr::vector<std::pmr::string> redis_key_scan_args(std::string_view command,
    std::string_view key, const redis_scan_options& options, std::pmr::memory_resource* resource) {
    auto cursor_value = detail::redis_cursor_string(options.cursor_, resource);
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(7);
    detail::emplace_redis_string(args, command);
    detail::emplace_redis_string(args, key);
    args.emplace_back(std::move(cursor_value));
    detail::append_redis_scan_options(args, options, resource);
    return args;
}

task<redis_scan_result> execute_redis_scan(detail::redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value =
        co_await detail::execute_owned_redis_command(std::move(executor), std::move(args), resource);
    co_return detail::parse_redis_scan_result(value, resource);
}

task<redis_hash_scan_result> execute_redis_hash_scan(detail::redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value =
        co_await detail::execute_owned_redis_command(std::move(executor), std::move(args), resource);
    co_return detail::parse_redis_hash_scan_result(value, resource);
}

task<redis_z_scan_result> execute_redis_z_scan(detail::redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value =
        co_await detail::execute_owned_redis_command(std::move(executor), std::move(args), resource);
    co_return detail::parse_redis_z_scan_result(value, resource);
}

[[nodiscard]] std::chrono::milliseconds redis_block_client_timeout(
    std::chrono::milliseconds timeout) noexcept {
    constexpr auto grace = std::chrono::seconds(1);
    if (timeout > std::chrono::milliseconds::max() - grace) {
        return std::chrono::milliseconds::max();
    }
    return timeout + grace;
}

[[nodiscard]] operation_options redis_blocking_operation_options(
    operation_options options, redis_block_wait wait) {
    if (const auto duration = wait.duration(); duration.has_value()) {
        const auto client_timeout = redis_block_client_timeout(*duration);
        if (!options.timeout_.has_value() || client_timeout < *options.timeout_) {
            options.timeout_ = client_timeout;
        }
    } else if (!options.stop_token_.stoppable() && !options.timeout_.has_value()) {
        throw std::invalid_argument(
            "infinite redis block requires a stop_token or finite operation timeout");
    }
    return options;
}

task<std::optional<redis_key_value>> execute_redis_blocking_pop(detail::redis_pool& pool,
    std::pmr::vector<std::pmr::string> args, operation_options options,
    std::pmr::memory_resource* resource) {
    auto reply = co_await detail::execute_owned_redis_command(
        pool, std::move(args), std::move(options), resource);
    co_return detail::parse_redis_blocking_pop_reply(reply, resource);
}

task<std::optional<redis_x_read_group_result>> execute_redis_x_read_group(detail::redis_pool& pool,
    std::pmr::vector<std::pmr::string> args, operation_options options,
    std::pmr::memory_resource* resource) {
    auto reply = co_await detail::execute_owned_redis_command(
        pool, std::move(args), std::move(options), resource);
    co_return detail::parse_redis_x_read_group_reply(reply, resource);
}

}  // namespace

scoped_operation<redis_scan_result> redis_handle::scan(redis_scan_options options) const {
    registration_.require_active();
    return scoped(
        execute_redis_scan(executor(), redis_scan_args("SCAN", options, resource_), resource_));
}

scoped_operation<redis_hash_scan_result> redis_handle::hscan(
    std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return scoped(execute_redis_hash_scan(
        executor(), redis_key_scan_args("HSCAN", key, options, resource_), resource_));
}

scoped_operation<redis_scan_result> redis_handle::sscan(
    std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return scoped(execute_redis_scan(
        executor(), redis_key_scan_args("SSCAN", key, options, resource_), resource_));
}

scoped_operation<redis_z_scan_result> redis_handle::zscan(
    std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return scoped(execute_redis_z_scan(
        executor(), redis_key_scan_args("ZSCAN", key, options, resource_), resource_));
}

scoped_operation<redis_value> redis_handle::eval(std::string_view script,
    std::span<const std::string_view> keys, std::span<const std::string_view> args) const {
    registration_.require_active();
    return scoped(detail::execute_owned_redis_command(
        executor(), detail::redis_eval_args("EVAL", script, keys, args, resource_), resource_));
}

scoped_operation<redis_value> redis_handle::eval_sha(std::string_view sha1,
    std::span<const std::string_view> keys, std::span<const std::string_view> args) const {
    registration_.require_active();
    return scoped(detail::execute_owned_redis_command(
        executor(), detail::redis_eval_args("EVALSHA", sha1, keys, args, resource_), resource_));
}

scoped_operation<std::pmr::string> redis_handle::script_load(std::string_view script) const {
    registration_.require_active();
    return scoped(detail::redis_status_command(
        executor(), detail::own_redis_args({"SCRIPT", "LOAD", script}, resource_), resource_));
}

scoped_operation<std::pmr::vector<bool>> redis_handle::script_exists(
    std::span<const std::string_view> sha1s) const {
    registration_.require_active();
    if (sha1s.empty()) {
        throw std::invalid_argument("redis script exists requires at least one sha1");
    }
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(sha1s.size() + 2);
    detail::emplace_redis_string(args, "SCRIPT");
    detail::emplace_redis_string(args, "EXISTS");
    for (const auto sha1 : sha1s) {
        detail::emplace_redis_string(args, sha1);
    }
    return scoped(detail::redis_bool_array_command(executor(), std::move(args), resource_));
}

scoped_operation<std::optional<redis_key_value>> redis_handle::blpop(
    std::span<const std::string_view> keys, redis_block_wait wait) const {
    registration_.require_active();
    return scoped(execute_redis_blocking_pop(*blocking_pool_,
        detail::redis_blocking_pop_args("BLPOP", keys, wait, resource_),
        redis_blocking_operation_options(operation_options_, wait), resource_));
}

scoped_operation<std::optional<redis_key_value>> redis_handle::brpop(
    std::span<const std::string_view> keys, redis_block_wait wait) const {
    registration_.require_active();
    return scoped(execute_redis_blocking_pop(*blocking_pool_,
        detail::redis_blocking_pop_args("BRPOP", keys, wait, resource_),
        redis_blocking_operation_options(operation_options_, wait), resource_));
}

scoped_operation<std::optional<redis_x_read_group_result>> redis_handle::xread_group(
    std::string_view group, std::string_view consumer, std::span<const redis_stream_read_view> streams,
    redis_x_read_group_options options) const {
    registration_.require_active();
    auto operation = operation_options_;
    auto args = detail::redis_x_read_group_args(group, consumer, streams, options, resource_);
    auto* selected_pool = pool_;
    if (options.block_.has_value()) {
        selected_pool = blocking_pool_;
        operation = redis_blocking_operation_options(std::move(operation), *options.block_);
    }
    return scoped(
        execute_redis_x_read_group(*selected_pool, std::move(args), std::move(operation), resource_));
}

redis_pipeline redis_handle::pipeline() const {
    registration_.require_active();
    return redis_pipeline(*pool_, operation_options_, resource_, registration_.scope());
}

redis_transaction redis_handle::transaction() const {
    registration_.require_active();
    return redis_transaction(*pool_, operation_options_, resource_, registration_.scope());
}

}  // namespace ruvia
