#include "ruvia/web/db/Db.h"

#include <utility>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/db/DbPreparedStatement.h"
#include "ruvia/web/detail/db/DbUtils.h"

// An open transaction: it owns a pooled connection until commit, rollback, or
// destruction, and a transaction abandoned by an unwinding scope must roll back
// rather than leak the connection.

namespace ruvia {
namespace {

void abortPoolTransaction(detail::DbPoolRef pool, std::size_t slot) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        (*client)->abortTransaction(slot);
        return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        (*client)->abortTransaction(slot);
    }
#endif
}

}  // namespace

DbTransaction::Lease::Lease(
    detail::DbPoolRef client,
    std::size_t slot,
    std::pmr::memory_resource* resource) noexcept
    : client(client),
      slot(slot),
      resource(detail::pmrResourceOrDefault(resource)) {}

DbTransaction::DbTransaction(
    detail::DbPoolRef client,
    std::size_t slot,
    std::pmr::memory_resource* resource) noexcept
    : state_(Lease{client, slot, resource}) {}

DbTransaction::DbTransaction(DbTransaction&& other) noexcept
    : detail::ScopedCapabilityNode(std::move(other)),
      state_(std::move(other.state_)) {}

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

ScopedOperation<QueryResult> DbTransaction::query(
    std::string_view sql,
    std::span<const DbValue> params) {
    return execute(sql, params);
}

ScopedOperation<QueryResult> DbTransaction::execute(
    std::string_view sql,
    std::span<const DbValue> params) {
    // executePrepared performs the authoritative admission check when its lazy
    // task starts. Preparing parameters only needs the transaction's stable
    // request memory domain while the lease is idle.
    requireActive();
    auto statement = prepareDbStatement(
        sql, params, state_.activePayload().resource);
    return detail::makeScopedOperation(
        operationScope_,
        executePrepared(std::move(statement.sql), std::move(statement.params)));
}

Task<QueryResult> DbTransaction::executePrepared(
    std::pmr::string sql,
    std::pmr::vector<DbValue> params) {
    OperationGuard operation(*this);
    auto& lease = operation.lease();
    auto result = co_await executeTransactionPool(
        lease.client,
        lease.slot,
        std::move(sql),
        std::move(params),
        lease.resource);
    operation.finishActive();
    co_return result;
}

ScopedOperation<void> DbTransaction::commit() {
    requireActive();
    return detail::makeScopedOperation(operationScope_, commitTask());
}

Task<void> DbTransaction::commitTask() {
    OperationGuard operation(*this);
    auto& lease = operation.lease();
    co_await commitPoolTransaction(lease.client, lease.slot, lease.resource);
    operation.finishClosed();
}

ScopedOperation<void> DbTransaction::rollback() {
    requireActive();
    return detail::makeScopedOperation(operationScope_, rollbackTask());
}

Task<void> DbTransaction::rollbackTask() {
    OperationGuard operation(*this);
    auto& lease = operation.lease();
    co_await rollbackPoolTransaction(lease.client, lease.slot, lease.resource);
    operation.finishClosed();
}

void DbTransaction::reset() noexcept {
    state_.reset([](Lease& lease) noexcept {
        abortPoolTransaction(lease.client, lease.slot);
    });
}

DbTransaction::OperationGuard::OperationGuard(DbTransaction& owner)
    : owner_(&owner),
      lease_(&owner.state_.begin()) {}

DbTransaction::OperationGuard::~OperationGuard() {
    if (owner_ != nullptr) {
        owner_->state_.finishFailed();
    }
}

void DbTransaction::OperationGuard::finishActive() noexcept {
    owner_->state_.finishActive();
    owner_ = nullptr;
}

void DbTransaction::OperationGuard::finishClosed() noexcept {
    owner_->state_.finishClosed();
    owner_ = nullptr;
}

void DbTransaction::OperationGuard::finishFailed() noexcept {
    owner_->state_.finishFailed();
    owner_ = nullptr;
}

}  // namespace ruvia

}  // namespace ruvia
