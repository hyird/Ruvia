#include "ruvia/web/detail/db/DbRegistry.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/db/DbQueryCacheState.h"
#include "ruvia/web/detail/db/DbUtils.h"

namespace ruvia {
namespace {

[[nodiscard]] detail::DbPoolRef poolRef(const detail::DbRegistry::PoolOwner& owner) noexcept {
    return std::visit(
        [](const auto& value) -> detail::DbPoolRef {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::monostate>) {
                return {};
            } else {
                return value == nullptr ? detail::DbPoolRef{} : detail::DbPoolRef{value.get()};
            }
        },
        owner);
}

Task<void> connectPool(detail::DbPoolRef pool) {
    return detail::visitDbPool(pool, [](auto& client) { return client.connect(); });
}

void closePool(detail::DbPoolRef pool) noexcept {
    detail::visitDbPoolIfPresent(pool, [](auto& client) noexcept { client.closeNow(); });
}

}  // namespace

detail::DbRegistry::DbRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
    std::pmr::memory_resource* resource, const DbConfig& defaultConfig)
    : resource_(detail::pmrResourceOrDefault(resource)),
      entries_(resource_),
      aliasIndex_(resource_) {
    aliasIndex_.build({kDefaultCapabilityAlias});
    entries_.reserve(1);
    add(ioContext, worker, DbConfigStorage(defaultConfig, resource_));
}

detail::DbRegistry::DbRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
    std::pmr::memory_resource* resource, std::span<const detail::DbDefinition> databases)
    : resource_(detail::pmrResourceOrDefault(resource)),
      entries_(resource_),
      aliasIndex_(resource_) {
    validateCapabilityAliases(
        databases, "database alias must not be empty", "duplicate database alias");
    aliasIndex_.build(databases);
    entries_.reserve(databases.size());
    for (const auto& definition : databases) {
        add(ioContext, worker, DbConfigStorage(definition.config, resource_));
    }
}

detail::DbRegistry::~DbRegistry() = default;

void detail::DbRegistry::add(
    asio::io_context& ioContext, const WorkerHandle& worker, DbConfigStorage config) {
    std::unique_ptr<DbQueryCacheState, PmrObjectDeleter<DbQueryCacheState>> cache;
    if (config.cache) {
        cache = makePmrObject<DbQueryCacheState>(resource_, ioContext, worker, *config.cache, resource_);
    }
    config.cache.reset();
    PoolOwner owner;
    switch (config.driver) {
        case DbDriver::kUnspecified:
            std::terminate();
        case DbDriver::kMariaDb:
#ifdef RUVIA_ENABLE_MARIADB
            owner = detail::makePmrObject<MariaDbPool>(
                resource_, ioContext, worker, std::move(config), resource_);
            break;
#else
            std::terminate();
#endif
        case DbDriver::kPostgreSql:
#ifdef RUVIA_ENABLE_POSTGRESQL
            owner = detail::makePmrObject<PostgreSqlPool>(
                resource_, ioContext, worker, std::move(config), resource_);
            break;
#else
            std::terminate();
#endif
    }

    entries_.push_back(Entry{std::move(owner), std::move(cache)});
}

Task<void> detail::DbRegistry::connect() {
    for (auto& entry : entries_) {
        co_await connectPool(poolRef(entry.pool));
        if (entry.cache) {
            co_await entry.cache->connect();
        }
    }
}

void detail::DbRegistry::closeNow() noexcept {
    for (auto& entry : entries_) {
        if (entry.cache) {
            entry.cache->closeNow();
        }
        closePool(poolRef(entry.pool));
    }
}

bool detail::DbRegistry::empty() const noexcept {
    return entries_.empty();
}

DbHandle detail::DbRegistry::get(ScopedOperationScope& operationScope) const {
    const auto defaultPoolIndex = aliasIndex_.defaultIndex();
    if (!defaultPoolIndex.has_value()) {
        throw DbError(
            DbError::Code::kNotConfigured, std::nullopt, "default database is not configured");
    }
    return DbHandle(poolRef(entries_[*defaultPoolIndex].pool), resource_, operationScope, entries_[*defaultPoolIndex].cache.get());
}

DbHandle detail::DbRegistry::get(
    std::string_view alias, ScopedOperationScope& operationScope) const {
    const auto match = aliasIndex_.find(alias);
    if (match.has_value()) {
        return DbHandle(poolRef(entries_[*match].pool), resource_, operationScope, entries_[*match].cache.get());
    }
    throw DbError(DbError::Code::kNotConfigured, std::nullopt, "database is not configured");
}

}  // namespace ruvia
