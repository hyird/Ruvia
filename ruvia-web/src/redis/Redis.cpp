#include "ruvia/web/redis/Redis.h"

#include "ruvia/web/detail/redis/RedisConfigValidation.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/integration/DataAccessDefinitions.h"
#include <algorithm>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace ruvia {
namespace detail {

RedisRegistry::RedisRegistry(asio::io_context& ioContext, std::pmr::memory_resource* resource, std::span<const RedisDefinition> redis, const WorkerHandle* worker)
    : resource_(detail::pmrResourceOrDefault(resource)),
      pools_(resource_),
      aliasIndex_(resource_) {
    pools_.reserve(redis.size());
    for (const auto& definition : redis) {
        if (definition.alias.empty()) {
            throw std::invalid_argument("redis alias must not be empty");
        }
        if (std::ranges::any_of(pools_, [&definition](const Entry& entry) { return std::string_view(entry.alias) == std::string_view(definition.alias); })) {
            throw std::invalid_argument("duplicate redis alias");
        }
        auto generalConfig = cloneRedisConfig(definition.config, resource_);
        auto blockingConfig = cloneRedisConfig(definition.config, resource_);
        validateRedisConfig(generalConfig);
        const auto generalSize = generalConfig.poolSizePerWorker;
        const auto blockingSize = generalConfig.blockingPoolSizePerWorker;
        // Redis blocking commands own their wait semantics. Typed finite waits
        // install a per-operation deadline with protocol grace, while infinite
        // waits require an explicit StopToken or operation timeout. Inheriting
        // the ordinary pool's command timeout would cut long waits short and
        // repeatedly discard/reconnect BLOCK 0 sockets.
        blockingConfig.commandTimeout = std::nullopt;
        auto general = makePmrObject<RedisPool>(resource_, ioContext, std::move(generalConfig), generalSize, resource_, worker);
        auto blocking = makePmrObject<RedisPool>(resource_, ioContext, std::move(blockingConfig), blockingSize, resource_, worker);
        pools_.push_back(Entry{std::pmr::string(definition.alias, resource_), std::move(general), std::move(blocking)});
        if (std::string_view(pools_.back().alias) == kDefaultRedisAlias) {
            defaultPoolIndex_ = pools_.size() - 1;
        }
    }
    aliasIndex_.resize(pools_.size());
    for (std::size_t index = 0; index < aliasIndex_.size(); ++index) {
        aliasIndex_[index] = index;
    }
    std::ranges::sort(aliasIndex_, {}, [this](std::size_t index) -> std::string_view { return pools_[index].alias; });
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

bool RedisRegistry::needsDeadlineScan() const noexcept {
    return std::ranges::any_of(pools_, [](const Entry& entry) { return entry.general->needsDeadlineScan() || entry.blocking->needsDeadlineScan(); });
}

RedisHandle RedisRegistry::get(std::pmr::memory_resource* resource, ScopedOperationScope& operationScope) const {
    if (!defaultPoolIndex_.has_value()) {
        throw RedisError(RedisError::Code::kNotConfigured, "default redis is not configured");
    }
    auto& entry = pools_[*defaultPoolIndex_];
    return RedisHandle(*entry.general, *entry.blocking, resource, operationScope);
}

RedisHandle RedisRegistry::get(std::string_view alias, std::pmr::memory_resource* resource, ScopedOperationScope& operationScope) const {
    const auto match = std::ranges::lower_bound(aliasIndex_, alias, {}, [this](std::size_t index) -> std::string_view { return pools_[index].alias; });
    if (match != aliasIndex_.end() && std::string_view(pools_[*match].alias) == alias) {
        auto& entry = pools_[*match];
        return RedisHandle(*entry.general, *entry.blocking, resource, operationScope);
    }
    throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
}

void RedisRegistry::scanDeadlines() noexcept {
    const auto now = std::chrono::steady_clock::now();
    for (auto& entry : pools_) {
        entry.general->scanDeadlines(now);
        entry.blocking->scanDeadlines(now);
    }
}

}  // namespace detail
}  // namespace ruvia
