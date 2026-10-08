#include <utility>

#include "ruvia/web/db/Db.h"
#include "ruvia/web/db/DbQuery.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include "db/DbPreparedStatement.h"
#include "db/DbQueryCacheState.h"
#include "db/DbRegistry.h"

// An open transaction: it owns a pooled connection until commit, rollback, or
// destruction, and a transaction abandoned by an unwinding scope must roll back
// rather than leak the connection.

namespace ruvia {
namespace {

Task<DbRows> queryTransactionPool(detail::DbPoolRef pool, std::size_t slot, std::pmr::string sql,
    std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource,
    const OperationOptions& options) {
    return detail::visitDbPool(pool, [&](auto& client) {
        return client.queryOnTransactionSlot(
            slot, std::move(sql), std::move(params), resource, options);
    });
}

Task<DbExecResult> executeTransactionPool(detail::DbPoolRef pool, std::size_t slot,
    std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource,
    const OperationOptions& options) {
    return detail::visitDbPool(pool, [&](auto& client) {
        return client.executeOnTransactionSlot(
            slot, std::move(sql), std::move(params), resource, options);
    });
}

Task<void> commitPoolTransaction(detail::DbPoolRef pool, std::size_t slot,
    std::pmr::memory_resource* resource, const OperationOptions& options) {
    return detail::visitDbPool(
        pool, [&](auto& client) { return client.commitTransaction(slot, resource, options); });
}

Task<void> rollbackPoolTransaction(detail::DbPoolRef pool, std::size_t slot,
    std::pmr::memory_resource* resource, const OperationOptions& options) {
    return detail::visitDbPool(
        pool, [&](auto& client) { return client.rollbackTransaction(slot, resource, options); });
}

void abortPoolTransaction(detail::DbPoolRef pool, std::size_t slot) noexcept {
    detail::visitDbPoolIfPresent(
        pool, [&](auto& client) noexcept { client.abortTransaction(slot); });
}

}  // namespace

DbTransaction::Lease::Lease(detail::DbPoolRef client, std::size_t slot,
    std::pmr::memory_resource* resource, OperationOptions options) noexcept
    : client(client),
      slot(slot),
      resource(detail::pmrResourceOrDefault(resource)),
      options(std::move(options)) {}

class DbTransaction::State final {
public:
    State(detail::DbPoolRef client, std::size_t slot, std::pmr::memory_resource* resource,
        OperationOptions options) noexcept
        : operation(Lease{client, slot, resource, std::move(options)}) {}

    ~State() {
        operation.reset(
            [](Lease& lease) noexcept { abortPoolTransaction(lease.client, lease.slot); });
    }

    OperationState operation;
};

DbTransaction::DbTransaction(detail::DbPoolRef client, std::size_t slot,
    std::pmr::memory_resource* resource, OperationOptions options)
    : state_(detail::makePmrObject<State>(resource, client, slot, resource, std::move(options))) {}

DbTransaction::DbTransaction(DbTransaction&& other) noexcept
    : cache_(other.cache_),
      state_(std::move(other.state_)),
      registration_(std::move(other.registration_), this) {}

DbTransaction::~DbTransaction() = default;

bool DbTransaction::active() const noexcept {
    return state_ != nullptr && state_->operation.active();
}

void DbTransaction::bindOperationScope(::ruvia::operation_scope& scope) noexcept {
    registration_.bind(scope, this, &DbTransaction::expire_capability);
}

void DbTransaction::expire_capability(void* target) noexcept {
    auto& transaction = *static_cast<DbTransaction*>(target);
    transaction.reset();
}

DbDriver DbTransaction::queryDriver() const {
    registration_.require_active();
    return detail::dbPoolDriver(state_->operation.activePayload().client);
}

std::pmr::memory_resource* DbTransaction::queryResource() const {
    registration_.require_active();
    return state_->operation.activePayload().resource;
}

Task<DbRows> DbTransaction::queryTask(const DbQuery& query) {
    registration_.require_active();
    OperationGuard operation(state_->operation);
    const auto& lease = operation.lease();
    auto plan = detail::db_query_plan::prepare(query, nullptr,
        detail::dbPoolDriver(lease.client), lease.resource, cache_);
    return query_plan_prepared<false>(std::move(plan), cache_, std::move(operation));
}

Task<std::pair<DbRows, DbRows>> DbTransaction::queryAndCountTask(const DbQuery& query, const DbQuery& count) {
    registration_.require_active();
    OperationGuard operation(state_->operation);
    const auto& lease = operation.lease();
    auto plan = detail::db_query_plan::prepare(query, &count,
        detail::dbPoolDriver(lease.client), lease.resource, cache_);
    return query_plan_prepared<true>(std::move(plan), cache_, std::move(operation));
}

template <bool with_count>
Task<detail::db_query_result<with_count>> DbTransaction::query_plan_prepared(
    detail::db_query_plan plan, detail::DbQueryCacheState* cache, OperationGuard pending) {
    OperationGuard operation(std::move(pending));
    operation.start();
    auto& lease = operation.lease();
    bool backend_failed = false;
    try {
        auto result = co_await detail::execute_db_query_plan<with_count>(std::move(plan),
            detail::db_query_backend{lease.client, lease.slot, lease.resource, cache, &backend_failed},
            lease.options);
        operation.finishActive();
        co_return result;
    } catch (...) {
        // SQL failures have already retired the lease in the backend. Cache
        // failures must retire it here while it is still owned by this operation.
        if (!backend_failed) {
            abortPoolTransaction(lease.client, lease.slot);
        }
        throw;
    }
}

ScopedOperation<DbRows> DbTransaction::query(const DbQuery& query) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(), queryTask(query));
}

ScopedOperation<DbExecResult> DbTransaction::execute(const DbQuery& query) & {
    registration_.require_active();
    OperationGuard operation(state_->operation);
    const auto& lease = operation.lease();
    auto statement = query.compile(detail::dbPoolDriver(lease.client), lease.resource);
    if (statement.returnsRows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    return ::ruvia::make_scoped_operation(registration_.scope(),
        executePrepared(std::move(statement.sql_), std::move(statement.params_), std::move(operation)));
}

ScopedOperation<DbRows> DbTransaction::query(
    std::string_view sql, std::span<const DbValue> params) & {
    registration_.require_active();
    OperationGuard operation(state_->operation);
    auto statement = prepareDbStatement(sql, params, operation.lease().resource);
    return ::ruvia::make_scoped_operation(registration_.scope(),
        queryPrepared(std::move(statement.sql), std::move(statement.params), std::move(operation)));
}

ScopedOperation<DbExecResult> DbTransaction::execute(
    std::string_view sql, std::span<const DbValue> params) & {
    registration_.require_active();
    OperationGuard operation(state_->operation);
    auto statement = prepareDbStatement(sql, params, operation.lease().resource);
    return ::ruvia::make_scoped_operation(
        registration_.scope(), executePrepared(std::move(statement.sql), std::move(statement.params),
                                   std::move(operation)));
}

Task<DbRows> DbTransaction::queryPrepared(
    std::pmr::string sql, std::pmr::vector<DbValue> params, OperationGuard pending) {
    OperationGuard operation(std::move(pending));
    operation.start();
    auto& lease = operation.lease();
    auto result = co_await queryTransactionPool(
        lease.client, lease.slot, std::move(sql), std::move(params), lease.resource, lease.options);
    operation.finishActive();
    co_return result;
}

Task<DbExecResult> DbTransaction::executePrepared(
    std::pmr::string sql, std::pmr::vector<DbValue> params, OperationGuard pending) {
    OperationGuard operation(std::move(pending));
    operation.start();
    auto& lease = operation.lease();
    auto result = co_await executeTransactionPool(
        lease.client, lease.slot, std::move(sql), std::move(params), lease.resource, lease.options);
    operation.finishActive();
    co_return result;
}

ScopedOperation<void> DbTransaction::commit() & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), commitTask(OperationGuard(state_->operation)));
}

Task<void> DbTransaction::commitTask(OperationGuard pending) {
    OperationGuard operation(std::move(pending));
    operation.start();
    auto& lease = operation.lease();
    co_await commitPoolTransaction(lease.client, lease.slot, lease.resource, lease.options);
    operation.finishClosed();
}

ScopedOperation<void> DbTransaction::rollback() & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), rollbackTask(OperationGuard(state_->operation)));
}

Task<void> DbTransaction::rollbackTask(OperationGuard pending) {
    OperationGuard operation(std::move(pending));
    operation.start();
    auto& lease = operation.lease();
    co_await rollbackPoolTransaction(lease.client, lease.slot, lease.resource, lease.options);
    operation.finishClosed();
}

void DbTransaction::reset() noexcept {
    if (state_ != nullptr) {
        state_->operation.reset(
            [](Lease& lease) noexcept { abortPoolTransaction(lease.client, lease.slot); });
    }
}

}  // namespace ruvia
