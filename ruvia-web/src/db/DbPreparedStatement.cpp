#include "ruvia/web/detail/db/DbPreparedStatement.h"

#include <utility>

#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbUtils.h"

namespace ruvia {

PreparedDbStatement prepareDbStatement(
    std::string_view sql,
    std::span<const DbValue> params,
    std::pmr::memory_resource* resource) {
    return PreparedDbStatement{
        std::pmr::string(sql, resource),
        detail::cloneDbValues(params, resource)};
}

[[noreturn]] void throwUnavailableDbBackend() {
    throw std::logic_error("database backend is not available");
}

Task<QueryResult> executePool(
    detail::DbPoolRef pool,
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->execute(std::move(sql), std::move(params), resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->execute(std::move(sql), std::move(params), resource);
    }
#endif
    throwUnavailableDbBackend();
}

Task<DbStreamResult> streamPool(
    detail::DbPoolRef pool,
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->stream(std::move(sql), std::move(params), resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->stream(std::move(sql), std::move(params), resource);
    }
#endif
    throwUnavailableDbBackend();
}

Task<DbTransaction> beginPoolTransaction(
    detail::DbPoolRef pool,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->beginTransaction(resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->beginTransaction(resource);
    }
#endif
    throwUnavailableDbBackend();
}

}  // namespace ruvia
