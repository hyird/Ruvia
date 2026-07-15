#include "ruvia/web/db/Db.h"

#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/db/DbUtils.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <stdexcept>
#include <utility>

namespace ruvia {
namespace {

struct PreparedDbStatement final {
    std::pmr::string sql;
    std::pmr::vector<DbValue> params;
};

[[nodiscard]] PreparedDbStatement prepareDbStatement(
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

DbHandle::DbHandle(
    detail::DbPoolRef client,
    std::pmr::memory_resource* resource,
    detail::ScopedOperationScope& operationScope) noexcept
    : detail::ScopedCapabilityNode(operationScope, &DbHandle::expireCapability),
      client_(client),
      resource_(detail::pmrResourceOrDefault(resource)) {}

DbHandle::DbHandle(const DbHandle& other) noexcept
    : detail::ScopedCapabilityNode(other),
      client_(other.client_),
      resource_(other.resource_) {}

void DbHandle::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    auto& handle = static_cast<DbHandle&>(capability);
    handle.client_ = detail::DbPoolRef{};
    handle.resource_ = nullptr;
}

ScopedOperation<QueryResult> DbHandle::query(std::string_view sql, std::span<const DbValue> params) const {
    return execute(sql, params);
}

ScopedOperation<QueryResult> DbHandle::execute(std::string_view sql, std::span<const DbValue> params) const {
    requireActive();
    auto statement = prepareDbStatement(sql, params, resource_);
    return detail::makeScopedOperation(
        operationScope(),
        executePrepared(
            client_, std::move(statement.sql), std::move(statement.params),
            resource_));
}

Task<QueryResult> DbHandle::executePrepared(
    detail::DbPoolRef client,
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
    co_return co_await executePool(
        client, std::move(sql), std::move(params), resource);
}

ScopedOperation<DbStreamResult> DbHandle::queryStream(
    std::string_view sql,
    std::span<const DbValue> params) const {
    requireActive();
    auto statement = prepareDbStatement(sql, params, resource_);
    return detail::makeScopedOperation(
        operationScope(),
        queryStreamPrepared(
            client_, std::move(statement.sql), std::move(statement.params),
            resource_, operationScope()));
}

Task<DbStreamResult> DbHandle::queryStreamPrepared(
    detail::DbPoolRef client,
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource,
    detail::ScopedOperationScope& operationScope) {
    auto result = co_await streamPool(
        client, std::move(sql), std::move(params), resource);
    result.bindOperationScope(operationScope);
    co_return result;
}

ScopedOperation<DbTransaction> DbHandle::beginTransaction() const {
    requireActive();
    return detail::makeScopedOperation(
        operationScope(),
        beginTransactionPrepared(client_, resource_, operationScope()));
}

Task<DbTransaction> DbHandle::beginTransactionPrepared(
    detail::DbPoolRef client,
    std::pmr::memory_resource* resource,
    detail::ScopedOperationScope& operationScope) {
    auto transaction = co_await beginPoolTransaction(client, resource);
    transaction.bindOperationScope(operationScope);
    co_return transaction;
}

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
