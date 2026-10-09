#include <chrono>
#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/redis/redis_utils.h"
#include "ruvia/web/redis/redis.h"

#include "redis/redis_handle_command_ops.h"
#include "redis/redis_handle_helpers.h"
#include "redis/redis_registry.h"
#include "redis/redis_types_access.h"

namespace ruvia {

namespace {

void require_positive_redis_ttl(std::chrono::seconds ttl, const char* message) {
    if (ttl.count() <= 0) {
        throw std::invalid_argument(message);
    }
}

task<redis_ttl> redis_ttl_command(detail::redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource,
    bool seconds_precision) {
    const auto value =
        co_await detail::redis_integer_command(std::move(executor), std::move(args), resource);
    if (value == -2) {
        co_return detail::redis_types_access::ttl(redis_ttl_state::missing);
    }
    if (value == -1) {
        co_return detail::redis_types_access::ttl(redis_ttl_state::persistent);
    }
    if (value < 0) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid redis TTL reply");
    }

    using milliseconds_type = std::chrono::milliseconds;
    auto milliseconds = value;
    if (seconds_precision) {
        constexpr auto scale = milliseconds_type(std::chrono::seconds(1)).count();
        if (value > milliseconds_type::max().count() / scale) {
            throw redis_error(
                redis_error::code_type::protocol_error, "redis TTL reply exceeds milliseconds range");
        }
        milliseconds *= scale;
    }
    co_return detail::redis_types_access::ttl(redis_ttl_state::expiring, milliseconds_type(milliseconds));
}

}  // namespace

redis_handle::redis_handle(detail::redis_pool& general_pool, detail::redis_pool& blocking_pool_value,
    std::pmr::memory_resource* resource, ::ruvia::operation_scope& operation_scope) noexcept
    : pool_(&general_pool),
      blocking_pool_(&blocking_pool_value),
      resource_(detail::pmr_resource_or_default(resource)),
      registration_(operation_scope, this, &redis_handle::expire_capability) {}

redis_handle::redis_handle(detail::redis_pool& general_pool, detail::redis_pool& blocking_pool_value,
    std::pmr::memory_resource* resource, ::ruvia::operation_scope& operation_scope,
    operation_options options) noexcept
    : pool_(&general_pool),
      blocking_pool_(&blocking_pool_value),
      resource_(detail::pmr_resource_or_default(resource)),
      operation_options_(std::move(options)),
      registration_(operation_scope, this, &redis_handle::expire_capability) {}

redis_handle::redis_handle(const redis_handle& other) noexcept
    : pool_(other.pool_),
      blocking_pool_(other.blocking_pool_),
      resource_(other.resource_),
      operation_options_(other.operation_options_),
      registration_(other.registration_, this) {}

redis_handle redis_handle::with_options(operation_options options) const {
    registration_.require_active();
    detail::validate_operation_options(options);
    redis_handle configured(*this);
    configured.operation_options_ =
        detail::merge_operation_options(operation_options_, std::move(options));
    return configured;
}

const worker_handle& redis_handle::worker() const& {
    registration_.require_active();
    return pool_->worker_;
}

detail::redis_command_executor redis_handle::executor() const {
    return executor(*pool_);
}

detail::redis_command_executor redis_handle::executor(detail::redis_pool& pool) const {
    return detail::redis_command_executor{&pool, operation_options_};
}

void redis_handle::expire_capability(void* target) noexcept {
    auto& handle = *static_cast<redis_handle*>(target);
    handle.pool_ = nullptr;
    handle.blocking_pool_ = nullptr;
    handle.resource_ = nullptr;
    handle.operation_options_ = {};
}

scoped_operation<redis_value> redis_handle::command(std::span<const std::string_view> args) const {
    return scoped(command_owned(args));
}

task<redis_value> redis_handle::command_owned(std::span<const std::string_view> args) const {
    registration_.require_active();
    const bool blocking = detail::validate_redis_pooled_command(args, true);
    auto& selected_pool = blocking ? *blocking_pool_ : *pool_;
    if (blocking && !operation_options_.stop_token_.stoppable() &&
        !operation_options_.timeout_.has_value()) {
        throw std::invalid_argument(
            "raw blocking redis command requires a stop_token or finite operation timeout");
    }
    return detail::execute_owned_redis_command(
        selected_pool, detail::own_redis_args(args, resource_), operation_options_, resource_);
}

scoped_operation<void> redis_handle::ping() const {
    registration_.require_active();
    return scoped(
        detail::execute_redis_ping(executor(), detail::own_redis_args({"PING"}, resource_), resource_));
}

scoped_operation<std::pmr::string> redis_handle::ping(std::string_view message) const {
    registration_.require_active();
    return scoped(detail::redis_status_command(
        executor(), detail::own_redis_args({"PING", message}, resource_), resource_));
}

scoped_operation<std::optional<std::pmr::string>> redis_handle::get(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_string_command(
        executor(), detail::own_redis_args({"GET", key}, resource_), resource_));
}

scoped_operation<std::pmr::vector<std::optional<std::pmr::string>>> redis_handle::mget(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return scoped(detail::redis_optional_string_array_command(
        executor(), detail::redis_command_with_keys("MGET", keys, resource_), resource_));
}

scoped_operation<redis_set_result> redis_handle::set(
    std::string_view key, std::string_view value, redis_set_options options) const {
    registration_.require_active();
    auto args = detail::redis_set_args(key, value, options, resource_);
    return scoped(
        detail::execute_redis_set(executor(), std::move(args), std::move(options), resource_));
}

scoped_operation<void> redis_handle::mset(
    std::span<const std::pair<std::string_view, std::string_view>> items) const {
    registration_.require_active();
    return scoped(
        detail::redis_ok_command(executor(), detail::redis_mset_args(items, resource_), resource_));
}

scoped_operation<std::optional<std::pmr::string>> redis_handle::get_del(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_string_command(
        executor(), detail::own_redis_args({"GETDEL", key}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::append(
    std::string_view key, std::string_view value) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"APPEND", key, value}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::strlen(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"STRLEN", key}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::incr_by(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    auto amount = detail::redis_int_string(value, resource_);
    return scoped(detail::redis_integer_command(executor(),
        detail::own_redis_args({"INCRBY", key, std::string_view(amount)}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::decr(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"DECR", key}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::decr_by(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    auto amount = detail::redis_int_string(value, resource_);
    return scoped(detail::redis_integer_command(executor(),
        detail::own_redis_args({"DECRBY", key, std::string_view(amount)}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::del(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"DEL", key}, resource_), resource_));
}

scoped_operation<std::int64_t> redis_handle::unlink(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"UNLINK", key}, resource_), resource_));
}

scoped_operation<bool> redis_handle::exists(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::execute_redis_integer_bool(
        executor(), detail::own_redis_args({"EXISTS", key}, resource_), resource_));
}

scoped_operation<bool> redis_handle::touch(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::execute_redis_integer_bool(
        executor(), detail::own_redis_args({"TOUCH", key}, resource_), resource_));
}

scoped_operation<std::pmr::string> redis_handle::type(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_status_command(
        executor(), detail::own_redis_args({"TYPE", key}, resource_), resource_));
}

scoped_operation<void> redis_handle::rename(std::string_view key, std::string_view new_key) const {
    registration_.require_active();
    return scoped(detail::redis_ok_command(
        executor(), detail::own_redis_args({"RENAME", key, new_key}, resource_), resource_));
}

scoped_operation<bool> redis_handle::rename_nx(std::string_view key, std::string_view new_key) const {
    registration_.require_active();
    return scoped(detail::execute_redis_integer_bool(
        executor(), detail::own_redis_args({"RENAMENX", key, new_key}, resource_), resource_));
}

scoped_operation<bool> redis_handle::expire(std::string_view key, std::chrono::seconds ttl) const {
    registration_.require_active();
    require_positive_redis_ttl(ttl, "redis expire TTL must be greater than zero");
    auto ttl_value = detail::redis_seconds_string(ttl, resource_);
    return scoped(detail::execute_redis_integer_bool(executor(),
        detail::own_redis_args({"EXPIRE", key, std::string_view(ttl_value)}, resource_), resource_));
}

scoped_operation<bool> redis_handle::expire_at(
    std::string_view key, std::chrono::system_clock::time_point expires_at) const {
    registration_.require_active();
    auto value = detail::redis_milliseconds_string(
        std::chrono::ceil<std::chrono::milliseconds>(expires_at.time_since_epoch()), resource_);
    return scoped(detail::execute_redis_integer_bool(executor(),
        detail::own_redis_args({"PEXPIREAT", key, std::string_view(value)}, resource_), resource_));
}

scoped_operation<bool> redis_handle::persist(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::execute_redis_integer_bool(
        executor(), detail::own_redis_args({"PERSIST", key}, resource_), resource_));
}

scoped_operation<redis_ttl> redis_handle::ttl(std::string_view key) const {
    registration_.require_active();
    return scoped(redis_ttl_command(
        executor(), detail::own_redis_args({"TTL", key}, resource_), resource_, true));
}

scoped_operation<redis_ttl> redis_handle::pttl(std::string_view key) const {
    registration_.require_active();
    return scoped(redis_ttl_command(
        executor(), detail::own_redis_args({"PTTL", key}, resource_), resource_, false));
}

scoped_operation<std::int64_t> redis_handle::incr(std::string_view key) const {
    registration_.require_active();
    return scoped(detail::redis_integer_command(
        executor(), detail::own_redis_args({"INCR", key}, resource_), resource_));
}

}  // namespace ruvia
