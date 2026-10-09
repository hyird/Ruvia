#include "ruvia/web/redis/redis.h"

#include <optional>
#include <stdexcept>
#include <utility>

#include "redis/redis_registry.h"

namespace ruvia {
namespace detail {

redis_registry::redis_registry(asio::io_context& io_context, std::pmr::memory_resource* resource,
    std::span<const redis_definition_type> redis, worker_handle worker_value)
    : worker_(std::move(worker_value)),
      resource_(detail::pmr_resource_or_default(resource)),
      pools_(resource_),
      alias_index_(resource_) {
    if (!worker_.valid()) {
        throw std::invalid_argument("redis registry requires a valid worker");
    }
    validate_capability_aliases(redis, "redis alias must not be empty", "duplicate redis alias");
    alias_index_.build(redis);
    pools_.reserve(redis.size());
    for (const auto& definition : redis) {
        pools_.push_back(make_pmr_object<redis_client_runtime>(resource_, io_context, worker_,
            redis_config_storage(definition.config_, resource_), resource_));
    }
}

redis_registry::~redis_registry() = default;

task<void> redis_registry::connect() {
    for (auto& entry : pools_) {
        co_await entry->connect();
    }
    co_return;
}

void redis_registry::close_now() noexcept {
    for (auto& entry : pools_) {
        entry->close_now();
    }
}

bool redis_registry::empty() const noexcept {
    return pools_.empty();
}

redis_handle redis_registry::get(::ruvia::operation_scope& operation_scope) const {
    const auto default_pool_index = alias_index_.default_index();
    if (!default_pool_index.has_value()) {
        throw redis_error(redis_error::code_type::not_configured, "default redis is not configured");
    }
    return pools_[*default_pool_index]->handle(operation_scope);
}

redis_handle redis_registry::get(
    std::string_view alias, ::ruvia::operation_scope& operation_scope) const {
    const auto match = alias_index_.find(alias);
    if (match.has_value()) {
        return pools_[*match]->handle(operation_scope);
    }
    throw redis_error(redis_error::code_type::not_configured, "redis is not configured");
}

}  // namespace detail
}  // namespace ruvia
