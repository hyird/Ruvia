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
        pools_.emplace_back(definition.config, resource_);
        auto& entry = pools_.back();
        const auto generalSize = entry.config.poolSizePerWorker;
        const auto blockingSize = entry.config.blockingPoolSizePerWorker;
        // Redis blocking commands own their wait semantics. Typed finite waits
        // install a per-operation deadline with protocol grace, while infinite
        // waits require an explicit StopToken or operation timeout. Inheriting
        // the ordinary pool's command timeout would cut long waits short and
        // repeatedly discard/reconnect BLOCK 0 sockets.
        entry.general = makePmrObject<RedisPool>(resource_, ioContext, entry.config,
            entry.config.commandTimeout, generalSize, worker_, resource_);
        entry.blocking = makePmrObject<RedisPool>(
            resource_, ioContext, entry.config, std::nullopt, blockingSize, worker_, resource_);
    }
}

RedisRegistry::~RedisRegistry() = default;

Task<void> RedisRegistry::connect() {
    for (auto& entry : pools_) {
        // The ordinary pool is startup-validated eagerly. Blocking slots stay
        // disconnected until first use so the isolated capacity has no idle
        // server-connection cost for applications that never block.
        co_await entry.general->connect();
    }
    co_return;
}

void RedisRegistry::closeNow() noexcept {
    for (auto& entry : pools_) {
        entry.general->closeNow();
        entry.blocking->closeNow();
    }
}

bool RedisRegistry::empty() const noexcept {
    return pools_.empty();
}

RedisHandle RedisRegistry::get(
    std::pmr::memory_resource* resource, ScopedOperationScope& operationScope) const {
    const auto defaultPoolIndex = aliasIndex_.defaultIndex();
    if (!defaultPoolIndex.has_value()) {
        throw RedisError(RedisError::Code::kNotConfigured, "default redis is not configured");
    }
    auto& entry = pools_[*defaultPoolIndex];
    return RedisHandle(*entry.general, *entry.blocking, resource, operationScope);
}

RedisHandle RedisRegistry::get(std::string_view alias, std::pmr::memory_resource* resource,
    ScopedOperationScope& operationScope) const {
    const auto match = aliasIndex_.find(alias);
    if (match.has_value()) {
        auto& entry = pools_[*match];
        return RedisHandle(*entry.general, *entry.blocking, resource, operationScope);
    }
    throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
}

}  // namespace detail
}  // namespace ruvia
