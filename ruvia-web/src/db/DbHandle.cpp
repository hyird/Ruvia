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
    std::pmr::memory_resource* resource) noexcept
    : client_(client),
      resource_(detail::pmrResourceOrDefault(resource)) {}

Task<QueryResult> DbHandle::query(std::string_view sql, std::span<const DbValue> params) const {
    return execute(sql, params);
}

Task<QueryResult> DbHandle::execute(std::string_view sql, std::span<const DbValue> params) const {
    auto statement = prepareDbStatement(sql, params, resource_);
    return executePrepared(std::move(statement.sql), std::move(statement.params));
}

Task<QueryResult> DbHandle::executePrepared(
    std::pmr::string sql,
    std::pmr::vector<DbValue> params) const {
    co_return co_await executePool(
        client_, std::move(sql), std::move(params), resource_);
}

Task<DbStreamResult> DbHandle::queryStream(
    std::string_view sql,
    std::span<const DbValue> params) const {
    auto statement = prepareDbStatement(sql, params, resource_);
    return queryStreamPrepared(std::move(statement.sql), std::move(statement.params));
}

Task<DbStreamResult> DbHandle::queryStreamPrepared(
    std::pmr::string sql,
    std::pmr::vector<DbValue> params) const {
    return streamPool(client_, std::move(sql), std::move(params), resource_);
}

Task<DbTransaction> DbHandle::beginTransaction() const {
    return beginPoolTransaction(client_, resource_);
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
    : state_(std::move(other.state_)) {}

DbStreamResult::~DbStreamResult() {
    reset();
}

bool DbStreamResult::active() const noexcept {
    return state_.active();
}

Task<std::optional<DbRow>> DbStreamResult::read() {
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

Task<void> DbStreamResult::close() {
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
    : state_(std::move(other.state_)) {}

DbTransaction::~DbTransaction() {
    reset();
}

bool DbTransaction::active() const noexcept {
    return state_.active();
}

Task<QueryResult> DbTransaction::query(
    std::string_view sql,
    std::span<const DbValue> params) {
    return execute(sql, params);
}

Task<QueryResult> DbTransaction::execute(
    std::string_view sql,
    std::span<const DbValue> params) {
    // executePrepared performs the authoritative admission check when its lazy
    // task starts. Preparing parameters only needs the transaction's stable
    // request memory domain while the lease is idle.
    auto statement = prepareDbStatement(
        sql, params, state_.activePayload().resource);
    return executePrepared(std::move(statement.sql), std::move(statement.params));
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

Task<void> DbTransaction::commit() {
    OperationGuard operation(*this);
    auto& lease = operation.lease();
    co_await commitPoolTransaction(lease.client, lease.slot, lease.resource);
    operation.finishClosed();
}

Task<void> DbTransaction::rollback() {
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
