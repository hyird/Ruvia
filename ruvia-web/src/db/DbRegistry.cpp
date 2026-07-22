#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <chrono>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace ruvia {
namespace {

[[nodiscard]] detail::DbPoolRef poolRef(
    const detail::DbRegistry::PoolOwner& owner) noexcept {
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

[[nodiscard]] bool poolHasTimeout(detail::DbPoolRef pool) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->hasAnyTimeout();
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->hasAnyTimeout();
    }
#endif
    return false;
}

}  // namespace

detail::DbRegistry::DbRegistry(
    asio::io_context& ioContext,
    std::pmr::memory_resource* resource,
    std::span<const detail::DbDefinition> databases)
    : resource_(detail::pmrResourceOrDefault(resource)),
      clients_(resource_) {
    clients_.reserve(databases.size());
    for (const auto& definition : databases) {
        if (definition.alias.empty()) {
            throw std::invalid_argument("database alias must not be empty");
        }
        if (std::ranges::any_of(
                clients_,
                [&definition](const Entry& entry) {
                    return std::string_view(entry.alias) ==
                        std::string_view(definition.alias);
                })) {
            throw std::invalid_argument("duplicate database alias");
        }

        validateDbConfig(definition.config);
        PoolOwner owner;
        switch (definition.config.driver) {
            case DbDriver::kMariaDb:
#ifdef RUVIA_ENABLE_MARIADB
                owner = detail::makePmrObject<MariaDbPool>(
                    resource_, ioContext, definition.config, resource_);
                break;
#else
                throw std::invalid_argument("MariaDB support is not enabled");
#endif
            case DbDriver::kPostgreSql:
#ifdef RUVIA_ENABLE_POSTGRESQL
                owner = detail::makePmrObject<PostgreSqlPool>(
                    resource_, ioContext, definition.config, resource_);
                break;
#else
                throw std::invalid_argument("PostgreSQL support is not enabled");
#endif
        }

        clients_.push_back(Entry{
            std::pmr::string(definition.alias, resource_),
            std::move(owner)});
        if (std::string_view(clients_.back().alias) ==
            kDefaultDbAlias) {
            defaultClientIndex_ = clients_.size() - 1;
        }
    }
}

detail::DbRegistry::~DbRegistry() = default;

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

bool detail::DbRegistry::hasAnyTimeout() const noexcept {
    return std::ranges::any_of(clients_, [](const Entry& entry) {
        return poolHasTimeout(poolRef(entry.client));
    });
}

DbHandle detail::DbRegistry::get(
    std::pmr::memory_resource* resource,
    ScopedOperationScope& operationScope) const {
    if (!defaultClientIndex_.has_value()) {
        throw std::logic_error("default database is not configured");
    }
    return DbHandle(
        poolRef(clients_[*defaultClientIndex_].client),
        resource,
        operationScope);
}

DbHandle detail::DbRegistry::get(
    std::string_view alias,
    std::pmr::memory_resource* resource,
    ScopedOperationScope& operationScope) const {
    for (const auto& entry : clients_) {
        if (std::string_view(entry.alias) == alias) {
            return DbHandle(poolRef(entry.client), resource, operationScope);
        }
    }
    throw std::logic_error("database is not configured");
}

}  // namespace ruvia
