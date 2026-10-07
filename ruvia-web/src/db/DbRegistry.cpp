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
#include "ruvia/web/detail/redis/RedisRegistry.h"

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
    std::pmr::memory_resource* resource, std::span<const detail::DbDefinition> databases, RedisRegistry* redis)
    : resource_(detail::pmrResourceOrDefault(resource)),
      entries_(resource_),
      aliasIndex_(resource_) {
    validateCapabilityAliases(
        databases, "database alias must not be empty", "duplicate database alias");
    aliasIndex_.build(databases);
    entries_.reserve(databases.size());
    for (const auto& definition : databases) {
        add(ioContext, worker, DbConfigStorage(definition.config, resource_));
        if (definition.query_cache) {
#ifdef RUVIA_ENABLE_REDIS
            if (redis == nullptr) {
                throw std::invalid_argument("database query cache requires an existing Redis registry");
            }
            const auto store = redis->get(definition.query_cache->redis_alias, cache_scope_);
            attach_cache(entries_.size() - 1, worker, store, definition.query_cache->policy,
                definition.config, definition.alias);
#else
            (void)redis;
            throw std::invalid_argument("database query caching requires Redis support");
#endif
        }
    }
}

detail::DbRegistry::DbRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
    std::pmr::memory_resource* resource, const DbConfig& config,
    const RedisHandle& redis, const DbCacheConfig& policy)
    : DbRegistry(ioContext, worker, resource, config) {
    attach_cache(0, worker, redis, DbCacheConfigStorage(policy, resource_),
        DbConfigStorage(config, resource_), kDefaultCapabilityAlias);
}

void detail::DbRegistry::attach_cache(std::size_t index, const WorkerHandle& worker,
    const RedisHandle& redis, const DbCacheConfigStorage& policy,
    const DbConfigStorage& config, std::string_view alias) {
#ifdef RUVIA_ENABLE_REDIS
    if (redis.worker().id() != worker.id()) {
        throw std::invalid_argument("database query cache Redis capability must belong to the same worker");
    }
    const auto identity = db_cache_scope(policy.nameSpace, alias, config, resource_);
    entries_[index].cache = makePmrObject<DbQueryCacheState>(resource_, redis, policy, identity, resource_);
#else
    (void)index;
    (void)worker;
    (void)redis;
    (void)policy;
    (void)config;
    (void)alias;
    throw std::invalid_argument("database query caching requires Redis support");
#endif
}

detail::DbRegistry::~DbRegistry() = default;

void detail::DbRegistry::add(
    asio::io_context& ioContext, const WorkerHandle& worker, DbConfigStorage config) {
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

    entries_.push_back(Entry{std::move(owner), {}});
}

Task<void> detail::DbRegistry::connect() {
    for (auto& entry : entries_) {
        co_await connectPool(poolRef(entry.pool));
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

DbHandle detail::DbRegistry::get(::ruvia::operation_scope& operationScope) const {
    const auto defaultPoolIndex = aliasIndex_.defaultIndex();
    if (!defaultPoolIndex.has_value()) {
        throw DbError(
            DbError::Code::kNotConfigured, std::nullopt, "default database is not configured");
    }
    return DbHandle(poolRef(entries_[*defaultPoolIndex].pool), resource_, operationScope, entries_[*defaultPoolIndex].cache.get());
}

DbHandle detail::DbRegistry::get(
    std::string_view alias, ::ruvia::operation_scope& operationScope) const {
    const auto match = aliasIndex_.find(alias);
    if (match.has_value()) {
        return DbHandle(poolRef(entries_[*match].pool), resource_, operationScope, entries_[*match].cache.get());
    }
    throw DbError(DbError::Code::kNotConfigured, std::nullopt, "database is not configured");
}

}  // namespace ruvia
