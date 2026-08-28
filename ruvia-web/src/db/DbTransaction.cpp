#include "ruvia/web/db/Db.h"

#include <exception>
#include <utility>
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbPreparedStatement.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbUtils.h"

// An open transaction: it owns a pooled connection until commit, rollback, or
// destruction, and a transaction abandoned by an unwinding scope must roll back
// rather than leak the connection.

namespace ruvia {
namespace {

Task<DbRows> queryTransactionPool(detail::DbPoolRef pool, std::size_t slot, std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource, const OperationOptions& options) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->queryOnTransactionSlot(slot, std::move(sql), std::move(params), resource, options);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->queryOnTransactionSlot(slot, std::move(sql), std::move(params), resource, options);
    }
#endif
    detail::throwUnavailableDbBackend();
}

Task<DbExecResult> executeTransactionPool(detail::DbPoolRef pool, std::size_t slot, std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource, const OperationOptions& options) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->executeOnTransactionSlot(slot, std::move(sql), std::move(params), resource, options);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->executeOnTransactionSlot(slot, std::move(sql), std::move(params), resource, options);
    }
#endif
    detail::throwUnavailableDbBackend();
}

Task<void> commitPoolTransaction(detail::DbPoolRef pool, std::size_t slot, std::pmr::memory_resource* resource, const OperationOptions& options) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->commitTransaction(slot, resource, options);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->commitTransaction(slot, resource, options);
    }
#endif
    detail::throwUnavailableDbBackend();
}

Task<void> rollbackPoolTransaction(detail::DbPoolRef pool, std::size_t slot, std::pmr::memory_resource* resource, const OperationOptions& options) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->rollbackTransaction(slot, resource, options);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool); client != nullptr && *client != nullptr) {
        return (*client)->rollbackTransaction(slot, resource, options);
    }
#endif
    detail::throwUnavailableDbBackend();
}

void abortPoolTransaction(detail::DbPoolRef pool, std::size_t slot) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool); client != nullptr && *client != nullptr) {
        (*client)->abortTransaction(slot);
        return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool); client != nullptr && *client != nullptr) {
        (*client)->abortTransaction(slot);
    }
#endif
}

}  // namespace

DbTransaction::Lease::Lease(detail::DbPoolRef client, std::size_t slot, std::pmr::memory_resource* resource, OperationOptions options) noexcept
    : client(client),
      slot(slot),
      resource(detail::pmrResourceOrDefault(resource)),
      options(std::move(options)) {}

DbTransaction::DbTransaction(detail::DbPoolRef client, std::size_t slot, std::pmr::memory_resource* resource, OperationOptions options) noexcept
    : state_(Lease{client, slot, resource, std::move(options)}) {}

DbTransaction::DbTransaction(DbTransaction&& other) noexcept
    : detail::ScopedCapabilityNode(std::move(other)),
      state_(std::move(other.state_)) {
    if (other.operationScope_.hasPendingOperations()) {
        // executePrepared/commitTask/rollbackTask are member coroutines. A
        // cold frame retains the old `this`; moving the owner would otherwise
        // make the eventual await operate on the moved-from transaction.
        std::terminate();
    }
}

DbTransaction::~DbTransaction() {
    reset();
}

bool DbTransaction::active() const noexcept {
    return state_.active();
}

void DbTransaction::bindOperationScope(detail::ScopedOperationScope& scope) noexcept {
    bind(scope, &DbTransaction::expireCapability);
}

void DbTransaction::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    auto& transaction = static_cast<DbTransaction&>(capability);
    transaction.operationScope_.close();
    transaction.reset();
}

ScopedOperation<DbRows> DbTransaction::query(std::string_view sql, std::span<const DbValue> params) & {
    requireActive();
    auto statement = prepareDbStatement(sql, params, state_.activePayload().resource);
    return detail::makeScopedOperation(operationScope_, queryPrepared(std::move(statement.sql), std::move(statement.params)));
}

ScopedOperation<DbExecResult> DbTransaction::execute(std::string_view sql, std::span<const DbValue> params) & {
    // executePrepared performs the authoritative admission check when its lazy
    // task starts. Preparing parameters only needs the transaction's stable
    // request memory domain while the lease is idle.
    requireActive();
    auto statement = prepareDbStatement(sql, params, state_.activePayload().resource);
    return detail::makeScopedOperation(operationScope_, executePrepared(std::move(statement.sql), std::move(statement.params)));
}

Task<DbRows> DbTransaction::queryPrepared(std::pmr::string sql, std::pmr::vector<DbValue> params) {
    OperationGuard operation(*this);
    auto& lease = operation.lease();
    auto result = co_await queryTransactionPool(lease.client, lease.slot, std::move(sql), std::move(params), lease.resource, lease.options);
    operation.finishActive();
    co_return result;
}

Task<DbExecResult> DbTransaction::executePrepared(std::pmr::string sql, std::pmr::vector<DbValue> params) {
    OperationGuard operation(*this);
    auto& lease = operation.lease();
    auto result = co_await executeTransactionPool(lease.client, lease.slot, std::move(sql), std::move(params), lease.resource, lease.options);
    operation.finishActive();
    co_return result;
}

ScopedOperation<void> DbTransaction::commit() & {
    requireActive();
    return detail::makeScopedOperation(operationScope_, commitTask());
}

Task<void> DbTransaction::commitTask() {
    OperationGuard operation(*this);
    auto& lease = operation.lease();
    co_await commitPoolTransaction(lease.client, lease.slot, lease.resource, lease.options);
    operation.finishClosed();
}

ScopedOperation<void> DbTransaction::rollback() & {
    requireActive();
    return detail::makeScopedOperation(operationScope_, rollbackTask());
}

Task<void> DbTransaction::rollbackTask() {
    OperationGuard operation(*this);
    auto& lease = operation.lease();
    co_await rollbackPoolTransaction(lease.client, lease.slot, lease.resource, lease.options);
    operation.finishClosed();
}

void DbTransaction::reset() noexcept {
    state_.reset([](Lease& lease) noexcept { abortPoolTransaction(lease.client, lease.slot); });
}

}  // namespace ruvia
