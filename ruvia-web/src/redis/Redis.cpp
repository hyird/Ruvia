#include "ruvia/web/redis/Redis.h"

#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/redis/RedisRegistry.h"

namespace ruvia {
namespace detail {

RedisRegistry::RedisRegistry(asio::io_context& ioContext, std::pmr::memory_resource* resource,
    std::span<const RedisDefinition> redis, WorkerHandle worker)
    : worker_(std::move(worker)),
      resource_(detail::pmrResourceOrDefault(resource)),
      pools_(resource_),
      aliasIndex_(resource_) {
    if (!worker_.valid()) {
        throw std::invalid_argument("redis registry requires a valid worker");
    }
    validateCapabilityAliases(redis, "redis alias must not be empty", "duplicate redis alias");
    aliasIndex_.build(redis);
    pools_.reserve(redis.size());
    for (const auto& definition : redis) {
        pools_.push_back(makePmrObject<RedisClientRuntime>(resource_, ioContext, worker_,
            RedisConfigStorage(definition.config, resource_), resource_));
    }
}

RedisRegistry::~RedisRegistry() = default;

Task<void> RedisRegistry::connect() {
    for (auto& entry : pools_) {
        co_await entry->connect();
    }
    co_return;
}

void RedisRegistry::closeNow() noexcept {
    for (auto& entry : pools_) {
        entry->closeNow();
    }
}

bool RedisRegistry::empty() const noexcept {
    return pools_.empty();
}

RedisHandle RedisRegistry::get(ScopedOperationScope& operationScope) const {
    const auto defaultPoolIndex = aliasIndex_.defaultIndex();
    if (!defaultPoolIndex.has_value()) {
        throw RedisError(RedisError::Code::kNotConfigured, "default redis is not configured");
    }
    return pools_[*defaultPoolIndex]->handle(operationScope);
}

RedisHandle RedisRegistry::get(
    std::string_view alias, ScopedOperationScope& operationScope) const {
    const auto match = aliasIndex_.find(alias);
    if (match.has_value()) {
        return pools_[*match]->handle(operationScope);
    }
    throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
}

}  // namespace detail
}  // namespace ruvia
