#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

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
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        co_await (*client)->connect();
        co_return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        co_await (*client)->connect();
        co_return;
    }
#endif
    throw std::logic_error("database backend is not available");
}

void closePool(detail::DbPoolRef pool) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        (*client)->closeNow();
        return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        (*client)->closeNow();
    }
#endif
}

void scanPool(detail::DbPoolRef pool, std::chrono::steady_clock::time_point now) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        (*client)->scanDeadlines(now);
        return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        (*client)->scanDeadlines(now);
    }
#endif
}

[[nodiscard]] bool poolNeedsDeadlineScan(detail::DbPoolRef pool) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->needsDeadlineScan();
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->needsDeadlineScan();
    }
#endif
    return false;
}

}  // namespace

detail::DbRegistry::DbRegistry(asio::io_context& ioContext, std::pmr::memory_resource* resource,
    const DbConfig& defaultConfig, const WorkerHandle* worker)
    : resource_(detail::pmrResourceOrDefault(resource)),
      pools_(resource_),
      aliasIndex_(resource_) {
    aliasIndex_.build({kDefaultCapabilityAlias});
    pools_.reserve(1);
    add(ioContext, worker, DbConfigStorage(defaultConfig, resource_));
}

detail::DbRegistry::DbRegistry(asio::io_context& ioContext, std::pmr::memory_resource* resource,
    std::span<const detail::DbDefinition> databases, const WorkerHandle* worker)
    : resource_(detail::pmrResourceOrDefault(resource)),
      pools_(resource_),
      aliasIndex_(resource_) {
    validateCapabilityAliases(
        databases, "database alias must not be empty", "duplicate database alias");
    aliasIndex_.build(databases);
    pools_.reserve(databases.size());
    for (const auto& definition : databases) {
        add(ioContext, worker, DbConfigStorage(definition.config, resource_));
    }
}

detail::DbRegistry::~DbRegistry() = default;

void detail::DbRegistry::add(
    asio::io_context& ioContext, const WorkerHandle* worker, DbConfigStorage config) {
    PoolOwner owner;
    switch (config.driver) {
        case DbDriver::kUnspecified:
            std::terminate();
        case DbDriver::kMariaDb:
#ifdef RUVIA_ENABLE_MARIADB
            owner = detail::makePmrObject<MariaDbPool>(
                resource_, ioContext, std::move(config), resource_, worker);
            break;
#else
            std::terminate();
#endif
        case DbDriver::kPostgreSql:
#ifdef RUVIA_ENABLE_POSTGRESQL
            owner = detail::makePmrObject<PostgreSqlPool>(
                resource_, ioContext, std::move(config), resource_, worker);
            break;
#else
            std::terminate();
#endif
    }

    pools_.push_back(std::move(owner));
}

Task<void> detail::DbRegistry::connect() {
    for (auto& pool : pools_) {
        co_await connectPool(poolRef(pool));
    }
    co_return;
}

void detail::DbRegistry::closeNow() noexcept {
    for (auto& pool : pools_) {
        closePool(poolRef(pool));
    }
}

bool detail::DbRegistry::empty() const noexcept {
    return pools_.empty();
}

void detail::DbRegistry::scanDeadlines() noexcept {
    const auto now = std::chrono::steady_clock::now();
    for (auto& pool : pools_) {
        scanPool(poolRef(pool), now);
    }
}

bool detail::DbRegistry::needsDeadlineScan() const noexcept {
    return std::ranges::any_of(
        pools_, [](const PoolOwner& pool) { return poolNeedsDeadlineScan(poolRef(pool)); });
}

DbHandle detail::DbRegistry::get(
    std::pmr::memory_resource* resource, ScopedOperationScope& operationScope) const {
    const auto defaultPoolIndex = aliasIndex_.defaultIndex();
    if (!defaultPoolIndex.has_value()) {
        throw DbError(
            DbError::Code::kNotConfigured, std::nullopt, "default database is not configured");
    }
    return DbHandle(poolRef(pools_[*defaultPoolIndex]), resource, operationScope);
}

DbHandle detail::DbRegistry::get(std::string_view alias, std::pmr::memory_resource* resource,
    ScopedOperationScope& operationScope) const {
    const auto match = aliasIndex_.find(alias);
    if (match.has_value()) {
        return DbHandle(poolRef(pools_[*match]), resource, operationScope);
    }
    throw DbError(DbError::Code::kNotConfigured, std::nullopt, "database is not configured");
}

}  // namespace ruvia
