#include "ruvia/web/db/Db.h"

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

class DbTransaction::State final {
public:
    State(detail::DbPoolRef client, std::size_t slot, std::pmr::memory_resource* resource, OperationOptions options) noexcept
        : operation(Lease{client, slot, resource, std::move(options)}) {}

    ~State() {
        operationScope.close();
        operation.reset([](Lease& lease) noexcept { abortPoolTransaction(lease.client, lease.slot); });
    }

    OperationState operation;
    detail::ScopedOperationScope operationScope;
};

DbTransaction::DbTransaction(detail::DbPoolRef client, std::size_t slot, std::pmr::memory_resource* resource, OperationOptions options)
    : state_(detail::makePmrObject<State>(resource, client, slot, resource, std::move(options))) {}

DbTransaction::DbTransaction(DbTransaction&& other) noexcept
    : detail::ScopedCapabilityNode(std::move(other)),
      state_(std::move(other.state_)) {}

DbTransaction::~DbTransaction() = default;

bool DbTransaction::active() const noexcept {
    return state_ != nullptr && state_->operation.active();
}

void DbTransaction::bindOperationScope(detail::ScopedOperationScope& scope) noexcept {
    bind(scope, &DbTransaction::expireCapability);
}

void DbTransaction::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    auto& transaction = static_cast<DbTransaction&>(capability);
    if (transaction.state_ != nullptr) {
        transaction.state_->operationScope.close();
        transaction.reset();
    }
}

ScopedOperation<DbRows> DbTransaction::query(std::string_view sql, std::span<const DbValue> params) & {
    requireActive();
    OperationGuard operation(state_->operation);
    auto statement = prepareDbStatement(sql, params, operation.lease().resource);
    return detail::makeScopedOperation(state_->operationScope, queryPrepared(std::move(statement.sql), std::move(statement.params), std::move(operation)));
}

ScopedOperation<DbExecResult> DbTransaction::execute(std::string_view sql, std::span<const DbValue> params) & {
    requireActive();
    OperationGuard operation(state_->operation);
    auto statement = prepareDbStatement(sql, params, operation.lease().resource);
    return detail::makeScopedOperation(state_->operationScope, executePrepared(std::move(statement.sql), std::move(statement.params), std::move(operation)));
}

Task<DbRows> DbTransaction::queryPrepared(std::pmr::string sql, std::pmr::vector<DbValue> params, OperationGuard operation) {
    operation.start();
    auto& lease = operation.lease();
    auto result = co_await queryTransactionPool(lease.client, lease.slot, std::move(sql), std::move(params), lease.resource, lease.options);
    operation.finishActive();
    co_return result;
}

Task<DbExecResult> DbTransaction::executePrepared(std::pmr::string sql, std::pmr::vector<DbValue> params, OperationGuard operation) {
    operation.start();
    auto& lease = operation.lease();
    auto result = co_await executeTransactionPool(lease.client, lease.slot, std::move(sql), std::move(params), lease.resource, lease.options);
    operation.finishActive();
    co_return result;
}

ScopedOperation<void> DbTransaction::commit() & {
    requireActive();
    return detail::makeScopedOperation(state_->operationScope, commitTask(OperationGuard(state_->operation)));
}

Task<void> DbTransaction::commitTask(OperationGuard operation) {
    operation.start();
    auto& lease = operation.lease();
    co_await commitPoolTransaction(lease.client, lease.slot, lease.resource, lease.options);
    operation.finishClosed();
}

ScopedOperation<void> DbTransaction::rollback() & {
    requireActive();
    return detail::makeScopedOperation(state_->operationScope, rollbackTask(OperationGuard(state_->operation)));
}

Task<void> DbTransaction::rollbackTask(OperationGuard operation) {
    operation.start();
    auto& lease = operation.lease();
    co_await rollbackPoolTransaction(lease.client, lease.slot, lease.resource, lease.options);
    operation.finishClosed();
}

void DbTransaction::reset() noexcept {
    if (state_ != nullptr) {
        state_->operation.reset([](Lease& lease) noexcept { abortPoolTransaction(lease.client, lease.slot); });
    }
}

}  // namespace ruvia
