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
      clients_(resource_),
      aliasIndex_(resource_) {
    clients_.reserve(1);
    add(ioContext, worker, kDefaultDbAlias, DbConfigStorage(defaultConfig, resource_));
    buildAliasIndex();
}

detail::DbRegistry::DbRegistry(asio::io_context& ioContext, std::pmr::memory_resource* resource,
    std::span<const detail::DbDefinition> databases, const WorkerHandle* worker)
    : resource_(detail::pmrResourceOrDefault(resource)),
      clients_(resource_),
      aliasIndex_(resource_) {
    clients_.reserve(databases.size());
    for (const auto& definition : databases) {
        add(ioContext, worker, definition.alias, DbConfigStorage(definition.config, resource_));
    }
    buildAliasIndex();
}

detail::DbRegistry::~DbRegistry() = default;

void detail::DbRegistry::add(asio::io_context& ioContext, const WorkerHandle* worker,
    std::string_view alias, DbConfigStorage config) {
    if (alias.empty()) {
        throw std::invalid_argument("database alias must not be empty");
    }
    if (std::ranges::any_of(
            clients_, [alias](const Entry& entry) { return entry.alias == alias; })) {
        throw std::invalid_argument("duplicate database alias");
    }

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

    clients_.push_back(Entry{std::pmr::string(alias, resource_), std::move(owner)});
    if (std::string_view(clients_.back().alias) == kDefaultDbAlias) {
        defaultClientIndex_ = clients_.size() - 1;
    }
}

void detail::DbRegistry::buildAliasIndex() {
    aliasIndex_.resize(clients_.size());
    for (std::size_t index = 0; index < aliasIndex_.size(); ++index) {
        aliasIndex_[index] = index;
    }
    std::ranges::sort(aliasIndex_, {},
        [this](std::size_t index) -> std::string_view { return clients_[index].alias; });
}

Task<void> detail::DbRegistry::connect() {
    for (auto& entry : clients_) {
        co_await connectPool(poolRef(entry.client));
    }
    co_return;
}

void detail::DbRegistry::closeNow() noexcept {
    for (auto& entry : clients_) {
        closePool(poolRef(entry.client));
    }
}

bool detail::DbRegistry::empty() const noexcept {
    return clients_.empty();
}

void detail::DbRegistry::scanDeadlines() noexcept {
    const auto now = std::chrono::steady_clock::now();
    for (auto& entry : clients_) {
        scanPool(poolRef(entry.client), now);
    }
}

bool detail::DbRegistry::needsDeadlineScan() const noexcept {
    return std::ranges::any_of(
        clients_, [](const Entry& entry) { return poolNeedsDeadlineScan(poolRef(entry.client)); });
}

DbHandle detail::DbRegistry::get(
    std::pmr::memory_resource* resource, ScopedOperationScope& operationScope) const {
    if (!defaultClientIndex_.has_value()) {
        throw DbError(
            DbError::Code::kNotConfigured, std::nullopt, "default database is not configured");
    }
    return DbHandle(poolRef(clients_[*defaultClientIndex_].client), resource, operationScope);
}

DbHandle detail::DbRegistry::get(std::string_view alias, std::pmr::memory_resource* resource,
    ScopedOperationScope& operationScope) const {
    const auto match = std::ranges::lower_bound(aliasIndex_, alias, {},
        [this](std::size_t index) -> std::string_view { return clients_[index].alias; });
    if (match != aliasIndex_.end() && std::string_view(clients_[*match].alias) == alias) {
        return DbHandle(poolRef(clients_[*match].client), resource, operationScope);
    }
    throw DbError(DbError::Code::kNotConfigured, std::nullopt, "database is not configured");
}

}  // namespace ruvia
