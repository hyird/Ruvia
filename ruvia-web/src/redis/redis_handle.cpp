#include <chrono>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/redis/redis_repository_commands.h"
#include "ruvia/web/redis/redis.h"

#include "redis/redis_client_runtime.h"
#include "redis/redis_command_args.h"
#include "redis/redis_command_routing.h"
#include "redis/redis_pool.h"
#include "redis/redis_reply_parsers.h"

namespace ruvia {

namespace {

template <typename duration_type>
void require_positive_redis_ttl(duration_type ttl, const char* message) {
    if (ttl.count() <= 0) {
        throw std::invalid_argument(message);
    }
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
    std::optional<std::chrono::milliseconds> reply_timeout) const {
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
    registration_.require_active();
    return execute_mapped<void>(pool(), detail::make_owned_redis_command(resource_, {"PING"}), detail::parse_redis_pong);
}

task<std::pmr::string> redis_handle::ping(std::string_view message) const {
    registration_.require_active();
    return execute_mapped<std::pmr::string>(pool(), detail::make_owned_redis_command(resource_, {"PING", message}), detail::take_redis_string);
}

task<std::optional<std::pmr::string>> redis_handle::get(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::optional<std::pmr::string>>(pool(), detail::make_owned_redis_command(resource_, {"GET", key}), detail::parse_redis_optional_string);
}

task<std::pmr::vector<std::optional<std::pmr::string>>> redis_handle::mget(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::pmr::vector<std::optional<std::pmr::string>>>(pool(), detail::redis_command_with_keys("MGET", keys, resource_), detail::parse_redis_optional_string_array);
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
    registration_.require_active();
    return execute_mapped<std::optional<std::pmr::string>>(pool(), detail::make_owned_redis_command(resource_, {"GETDEL", key}), detail::parse_redis_optional_string);
}

task<std::int64_t> redis_handle::append(
    std::string_view key, std::string_view value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"APPEND", key, value}), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::strlen(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"STRLEN", key}), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::incr_by(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_redis_integer_command("INCRBY", key, value, resource_), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::decr(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"DECR", key}), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::decr_by(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_redis_integer_command("DECRBY", key, value, resource_), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::del(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"DEL", key}), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::del(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::redis_command_with_keys("DEL", keys, resource_), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::unlink(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"UNLINK", key}), detail::parse_redis_integer);
}

task<std::int64_t> redis_handle::unlink(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::redis_command_with_keys("UNLINK", keys, resource_), detail::parse_redis_integer);
}

task<bool> redis_handle::exists(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<bool>(pool(), detail::make_owned_redis_command(resource_, {"EXISTS", key}), detail::parse_redis_integer_bool);
}

task<bool> redis_handle::touch(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<bool>(pool(), detail::make_owned_redis_command(resource_, {"TOUCH", key}), detail::parse_redis_integer_bool);
}

task<std::pmr::string> redis_handle::type(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::pmr::string>(pool(), detail::make_owned_redis_command(resource_, {"TYPE", key}), detail::take_redis_string);
}

task<void> redis_handle::rename(std::string_view key, std::string_view new_key) const {
    registration_.require_active();
    return execute_mapped<void>(pool(), detail::make_owned_redis_command(resource_, {"RENAME", key, new_key}), detail::parse_redis_ok);
}

task<bool> redis_handle::rename_nx(std::string_view key, std::string_view new_key) const {
    registration_.require_active();
    return execute_mapped<bool>(pool(), detail::make_owned_redis_command(resource_, {"RENAMENX", key, new_key}), detail::parse_redis_integer_bool);
}

task<bool> redis_handle::expire(std::string_view key, std::chrono::seconds ttl) const {
    registration_.require_active();
    require_positive_redis_ttl(ttl, "redis expire TTL must be greater than zero");
    return execute_mapped<bool>(pool(), detail::make_redis_integer_command("EXPIRE", key, ttl.count(), resource_), detail::parse_redis_integer_bool);
}

task<bool> redis_handle::pexpire(std::string_view key, std::chrono::milliseconds ttl) const {
    registration_.require_active();
    require_positive_redis_ttl(ttl, "redis pexpire TTL must be greater than zero");
    return execute_mapped<bool>(pool(), detail::make_redis_integer_command("PEXPIRE", key, ttl.count(), resource_), detail::parse_redis_integer_bool);
}

task<bool> redis_handle::expire_at(
    std::string_view key, std::chrono::system_clock::time_point expires_at) const {
    registration_.require_active();
    const auto expires_ms =
        std::chrono::ceil<std::chrono::milliseconds>(expires_at.time_since_epoch()).count();
    return execute_mapped<bool>(pool(), detail::make_redis_integer_command("PEXPIREAT", key, expires_ms, resource_), detail::parse_redis_integer_bool);
}

task<bool> redis_handle::persist(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<bool>(pool(), detail::make_owned_redis_command(resource_, {"PERSIST", key}), detail::parse_redis_integer_bool);
}

task<redis_ttl> redis_handle::ttl(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<redis_ttl>(pool(), detail::make_owned_redis_command(resource_, {"TTL", key}),
        [](redis_value&& reply, std::pmr::memory_resource*) { return detail::parse_redis_ttl(std::move(reply), true); });
}

task<redis_ttl> redis_handle::pttl(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<redis_ttl>(pool(), detail::make_owned_redis_command(resource_, {"PTTL", key}),
        [](redis_value&& reply, std::pmr::memory_resource*) { return detail::parse_redis_ttl(std::move(reply), false); });
}

task<std::int64_t> redis_handle::incr(std::string_view key) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(resource_, {"INCR", key}), detail::parse_redis_integer);
}

}  // namespace ruvia
