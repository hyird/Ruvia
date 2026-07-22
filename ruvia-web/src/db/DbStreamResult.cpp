#include "ruvia/web/db/Db.h"

#include <utility>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/db/DbUtils.h"

// A streaming query result: the rows arrive one at a time from a pooled
// connection the result holds for as long as it is active, and every exit --
// exhaustion, explicit close, failure, or destruction -- must return that
// connection exactly once.

namespace ruvia {
namespace {

Task<std::optional<DbRow>> readPoolStream(
    detail::DbPoolRef pool,
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->readStreamRow(slot, result, resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->readStreamRow(slot, result, resource);
    }
#endif
    throwUnavailableDbBackend();
}

Task<void> closePoolStream(
    detail::DbPoolRef pool,
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->closeStream(slot, result, resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->closeStream(slot, result, resource);
    }
#endif
    throwUnavailableDbBackend();
}

void abortPoolStream(detail::DbPoolRef pool, std::size_t slot, void* result) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        (*client)->abortStream(slot, result);
        return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        (*client)->abortStream(slot, result);
    }
#endif
}

Task<QueryResult> executeTransactionPool(
    detail::DbPoolRef pool,
    std::size_t slot,
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->executeOnTransactionSlot(
            slot, std::move(sql), std::move(params), resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->executeOnTransactionSlot(
            slot, std::move(sql), std::move(params), resource);
    }
#endif
    throwUnavailableDbBackend();
}

Task<void> commitPoolTransaction(
    detail::DbPoolRef pool,
    std::size_t slot,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->commitTransaction(slot, resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->commitTransaction(slot, resource);
    }
#endif
    throwUnavailableDbBackend();
}

Task<void> rollbackPoolTransaction(
    detail::DbPoolRef pool,
    std::size_t slot,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->rollbackTransaction(slot, resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->rollbackTransaction(slot, resource);
    }
#endif
    throwUnavailableDbBackend();
}

}  // namespace

DbStreamResult::Lease::Lease(
    detail::DbPoolRef client,
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource) noexcept
    : client(client),
      slot(slot),
      result(result),
      resource(detail::pmrResourceOrDefault(resource)) {}

DbStreamResult::DbStreamResult(
    detail::DbPoolRef client,
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource) noexcept
    : state_(Lease{client, slot, result, resource}) {}

DbStreamResult::DbStreamResult(DbStreamResult&& other) noexcept
    : detail::ScopedCapabilityNode(std::move(other)),
      state_(std::move(other.state_)) {}

DbStreamResult::~DbStreamResult() {
    reset();
}

bool DbStreamResult::active() const noexcept {
    return state_.active();
}

void DbStreamResult::bindOperationScope(detail::ScopedOperationScope& scope) noexcept {
    bind(scope, &DbStreamResult::expireCapability);
}

void DbStreamResult::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    auto& result = static_cast<DbStreamResult&>(capability);
    result.operationScope_.close();
    result.reset();
}

ScopedOperation<std::optional<DbRow>> DbStreamResult::read() {
    requireActive();
    return detail::makeScopedOperation(operationScope_, readTask());
}

Task<std::optional<DbRow>> DbStreamResult::readTask() {
    OperationGuard operation(*this);
    auto& lease = operation.lease();
    auto row = co_await readPoolStream(
        lease.client, lease.slot, lease.result, lease.resource);
    if (row) {
        operation.finishActive();
    } else {
        operation.finishClosed();
    }
    co_return row;
}

ScopedOperation<void> DbStreamResult::close() {
    requireActive();
    return detail::makeScopedOperation(operationScope_, closeTask());
}

Task<void> DbStreamResult::closeTask() {
    OperationGuard operation(*this);
    auto& lease = operation.lease();
    co_await closePoolStream(
        lease.client, lease.slot, lease.result, lease.resource);
    operation.finishClosed();
}

void DbStreamResult::reset() noexcept {
    state_.reset([](Lease& lease) noexcept {
        abortPoolStream(lease.client, lease.slot, lease.result);
    });
}

DbStreamResult::OperationGuard::OperationGuard(DbStreamResult& owner)
    : owner_(&owner),
      lease_(&owner.state_.begin()) {}

DbStreamResult::OperationGuard::~OperationGuard() {
    if (owner_ != nullptr) {
        owner_->state_.finishFailed();
    }
}

void DbStreamResult::OperationGuard::finishActive() noexcept {
    owner_->state_.finishActive();
    owner_ = nullptr;
}

void DbStreamResult::OperationGuard::finishClosed() noexcept {
    owner_->state_.finishClosed();
    owner_ = nullptr;
}

void DbStreamResult::OperationGuard::finishFailed() noexcept {
    owner_->state_.finishFailed();
    owner_ = nullptr;
}

}  // namespace ruvia
