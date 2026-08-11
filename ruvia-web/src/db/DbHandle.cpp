#include "ruvia/web/db/Db.h"

#include <utility>
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbPreparedStatement.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbUtils.h"

// The per-request database handle: it borrows a registry entry for the
// operation's lifetime and hands each call to the pool.

namespace ruvia {

namespace {

Task<DbRows> queryPool(detail::DbPoolRef pool, std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource, OperationOptions options) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->query(std::move(sql), std::move(params), resource, std::move(options));
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->query(std::move(sql), std::move(params), resource, std::move(options));
    }
#endif
    detail::throwUnavailableDbBackend();
}

Task<DbExecResult> executePool(detail::DbPoolRef pool, std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource, OperationOptions options) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->execute(std::move(sql), std::move(params), resource, std::move(options));
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->execute(std::move(sql), std::move(params), resource, std::move(options));
    }
#endif
    detail::throwUnavailableDbBackend();
}

Task<DbStreamResult> streamPool(detail::DbPoolRef pool, std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource, OperationOptions options) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->stream(std::move(sql), std::move(params), resource, std::move(options));
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->stream(std::move(sql), std::move(params), resource, std::move(options));
    }
#endif
    detail::throwUnavailableDbBackend();
}

Task<DbTransaction> beginPoolTransaction(detail::DbPoolRef pool, std::pmr::memory_resource* resource, OperationOptions options) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->beginTransaction(resource, std::move(options));
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->beginTransaction(resource, std::move(options));
    }
#endif
    detail::throwUnavailableDbBackend();
}

}  // namespace

DbHandle::DbHandle(detail::DbPoolRef client, std::pmr::memory_resource* resource, detail::ScopedOperationScope& operationScope) noexcept
    : detail::ScopedCapabilityNode(operationScope, &DbHandle::expireCapability),
      client_(client),
      resource_(detail::pmrResourceOrDefault(resource)) {}

DbHandle::DbHandle(const DbHandle& other) noexcept
    : detail::ScopedCapabilityNode(other),
      client_(other.client_),
      resource_(other.resource_),
      options_(other.options_) {}

DbHandle DbHandle::withOptions(OperationOptions options) const {
    detail::validateOperationOptions(options);
    requireActive();
    DbHandle copy(*this);
    copy.options_ = detail::mergeOperationOptions(options_, std::move(options));
    return copy;
}

void DbHandle::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    auto& handle = static_cast<DbHandle&>(capability);
    handle.client_ = detail::DbPoolRef{};
    handle.resource_ = nullptr;
    handle.options_ = {};
}

ScopedOperation<DbRows> DbHandle::query(std::string_view sql, std::span<const DbValue> params) const {
    requireActive();
    auto statement = prepareDbStatement(sql, params, resource_);
    return detail::makeScopedOperation(
        operationScope(),
        queryPool(client_, std::move(statement.sql), std::move(statement.params), resource_, options_));
}

ScopedOperation<DbExecResult> DbHandle::execute(std::string_view sql, std::span<const DbValue> params) const {
    requireActive();
    auto statement = prepareDbStatement(sql, params, resource_);
    return detail::makeScopedOperation(
        operationScope(),
        executePool(client_, std::move(statement.sql), std::move(statement.params), resource_, options_));
}

ScopedOperation<DbStreamResult> DbHandle::queryStream(std::string_view sql, std::span<const DbValue> params) const {
    requireActive();
    auto statement = prepareDbStatement(sql, params, resource_);
    return detail::makeScopedOperation(operationScope(), queryStreamPrepared(client_, std::move(statement.sql), std::move(statement.params), resource_, operationScope(), options_));
}

Task<DbStreamResult> DbHandle::queryStreamPrepared(detail::DbPoolRef client, std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource, detail::ScopedOperationScope& operationScope, OperationOptions options) {
    auto result = co_await streamPool(client, std::move(sql), std::move(params), resource, std::move(options));
    result.bindOperationScope(operationScope);
    co_return result;
}

ScopedOperation<DbTransaction> DbHandle::beginTransaction() const {
    requireActive();
    return detail::makeScopedOperation(operationScope(), beginTransactionPrepared(client_, resource_, operationScope(), options_));
}

Task<DbTransaction> DbHandle::beginTransactionPrepared(detail::DbPoolRef client, std::pmr::memory_resource* resource, detail::ScopedOperationScope& operationScope, OperationOptions options) {
    auto transaction = co_await beginPoolTransaction(client, resource, std::move(options));
    transaction.bindOperationScope(operationScope);
    co_return transaction;
}

}  // namespace ruvia
