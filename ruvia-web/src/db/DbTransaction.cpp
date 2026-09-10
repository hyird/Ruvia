#include <utility>

#include "ruvia/web/db/Db.h"
#include "ruvia/web/db/DbQuery.h"
#include "ruvia/web/detail/db/DbPreparedStatement.h"
#include "ruvia/web/detail/db/DbQueryCacheState.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbUtils.h"

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
    : detail::ScopedCapabilityNode(std::move(other)),
      cache_(other.cache_),
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
    transaction.reset();
}

DbDriver DbTransaction::queryDriver() const {
    requireActive();
    return detail::dbPoolDriver(state_->operation.activePayload().client);
}

std::pmr::memory_resource* DbTransaction::queryResource() const {
    requireActive();
    return state_->operation.activePayload().resource;
}

Task<DbRows> DbTransaction::queryTask(const DbQuery& query) {
    requireActive();
    OperationGuard operation(state_->operation);
    const auto& lease = operation.lease();
    auto statement = query.compile(detail::dbPoolDriver(lease.client), lease.resource);
    if (!statement.returnsRows()) {
        throw std::invalid_argument("query requires a statement that returns rows");
    }
    if (cache_) {
        auto key = cache_->key(query, statement, detail::dbPoolDriver(lease.client));
        return queryCachedPrepared<false>(std::move(statement), std::nullopt, std::move(key), std::nullopt,
            query.cacheDuration(), std::nullopt, *cache_, operationScope(), std::move(operation));
    }
    return queryPrepared(std::move(statement.sql_), std::move(statement.params_), std::move(operation));
}

Task<std::pair<DbRows, DbRows>> DbTransaction::queryAndCountTask(const DbQuery& query, const DbQuery& count) {
    requireActive();
    OperationGuard operation(state_->operation);
    const auto& lease = operation.lease();
    const auto driver = detail::dbPoolDriver(lease.client);
    auto first = query.compile(driver, lease.resource);
    auto second = count.compile(driver, lease.resource);
    if (!first.returnsRows() || !second.returnsRows()) {
        throw std::invalid_argument("query and count require statements that return rows");
    }
    if (cache_) {
        auto firstKey = cache_->key(query, first, driver);
        auto secondKey = cache_->key(count, second, driver);
        return queryCachedPrepared<true>(std::move(first), std::move(second), std::move(firstKey), std::move(secondKey),
            query.cacheDuration(), count.cacheDuration(), *cache_, operationScope(), std::move(operation));
    }
    return queryAndCountPrepared(std::move(first), std::move(second), std::move(operation));
}

template <bool Count>
Task<std::conditional_t<Count, std::pair<DbRows, DbRows>, DbRows>> DbTransaction::queryCachedPrepared(
    DbStatement first, std::optional<DbStatement> second,
    std::optional<std::pmr::string> firstKey, std::optional<std::pmr::string> secondKey,
    std::optional<std::chrono::milliseconds> firstDuration,
    std::optional<std::chrono::milliseconds> secondDuration,
    detail::DbQueryCacheState& cache,
    detail::ScopedOperationScope& scope, OperationGuard operation) {
    operation.start();
    auto& lease = operation.lease();
    const detail::OperationTimeout operationTimeout(lease.options.timeout);
    bool backendFailed = false;
    try {
        auto firstOptions = detail::dbCacheRemainingOptions(lease.options, operationTimeout);
        auto rows = co_await cache.wrap(firstDuration, std::move(firstKey),
            detail::DbCacheQuery(lease.client, lease.slot, std::move(first.sql_),
                std::move(first.params_), lease.resource, &backendFailed),
            scope, std::move(firstOptions), operationTimeout);
        if (operationTimeout.expired()) {
            detail::throwDbCacheTimeout();
        }
        if constexpr (Count) {
            auto secondOptions = detail::dbCacheRemainingOptions(lease.options, operationTimeout);
            auto total = co_await cache.wrap(secondDuration, std::move(secondKey),
                detail::DbCacheQuery(lease.client, lease.slot, std::move(second->sql_),
                    std::move(second->params_), lease.resource, &backendFailed),
                scope, std::move(secondOptions), operationTimeout);
            if (operationTimeout.expired()) {
                detail::throwDbCacheTimeout();
            }
            operation.finishActive();
            co_return std::pair{std::move(rows), std::move(total)};
        } else {
            operation.finishActive();
            co_return rows;
        }
    } catch (...) {
        // SQL failures have already retired the lease in the backend. Cache
        // failures must retire it here while it is still owned by this operation.
        if (!backendFailed) {
            abortPoolTransaction(lease.client, lease.slot);
        }
        throw;
    }
}

Task<std::pair<DbRows, DbRows>> DbTransaction::queryAndCountPrepared(DbStatement query, DbStatement count, OperationGuard operation) {
    operation.start();
    auto& lease = operation.lease();
    const detail::OperationTimeout operationTimeout(lease.options.timeout);
    bool backendFailed = false;
    try {
        auto firstOptions = detail::dbCacheRemainingOptions(lease.options, operationTimeout);
        auto rows = co_await detail::DbCacheQuery(lease.client, lease.slot,
            std::move(query.sql_), std::move(query.params_), lease.resource, &backendFailed)(
            std::move(firstOptions));
        if (operationTimeout.expired()) {
            detail::throwDbCacheTimeout();
        }
        auto secondOptions = detail::dbCacheRemainingOptions(lease.options, operationTimeout);
        auto total = co_await detail::DbCacheQuery(lease.client, lease.slot,
            std::move(count.sql_), std::move(count.params_), lease.resource, &backendFailed)(
            std::move(secondOptions));
        if (operationTimeout.expired()) {
            detail::throwDbCacheTimeout();
        }
        operation.finishActive();
        co_return std::pair{std::move(rows), std::move(total)};
    } catch (...) {
        if (!backendFailed) {
            abortPoolTransaction(lease.client, lease.slot);
        }
        throw;
    }
}

ScopedOperation<DbRows> DbTransaction::query(const DbQuery& query) & {
    requireActive();
    return detail::makeScopedOperation(operationScope(), queryTask(query));
}

ScopedOperation<DbExecResult> DbTransaction::execute(const DbQuery& query) & {
    requireActive();
    OperationGuard operation(state_->operation);
    const auto& lease = operation.lease();
    auto statement = query.compile(detail::dbPoolDriver(lease.client), lease.resource);
    if (statement.returnsRows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    return detail::makeScopedOperation(operationScope(),
        executePrepared(std::move(statement.sql_), std::move(statement.params_), std::move(operation)));
}

ScopedOperation<DbRows> DbTransaction::query(
    std::string_view sql, std::span<const DbValue> params) & {
    requireActive();
    OperationGuard operation(state_->operation);
    auto statement = prepareDbStatement(sql, params, operation.lease().resource);
    return detail::makeScopedOperation(operationScope(),
        queryPrepared(std::move(statement.sql), std::move(statement.params), std::move(operation)));
}

ScopedOperation<DbExecResult> DbTransaction::execute(
    std::string_view sql, std::span<const DbValue> params) & {
    requireActive();
    OperationGuard operation(state_->operation);
    auto statement = prepareDbStatement(sql, params, operation.lease().resource);
    return detail::makeScopedOperation(
        operationScope(), executePrepared(std::move(statement.sql), std::move(statement.params),
                              std::move(operation)));
}

Task<DbRows> DbTransaction::queryPrepared(
    std::pmr::string sql, std::pmr::vector<DbValue> params, OperationGuard operation) {
    operation.start();
    auto& lease = operation.lease();
    auto result = co_await queryTransactionPool(
        lease.client, lease.slot, std::move(sql), std::move(params), lease.resource, lease.options);
    operation.finishActive();
    co_return result;
}

Task<DbExecResult> DbTransaction::executePrepared(
    std::pmr::string sql, std::pmr::vector<DbValue> params, OperationGuard operation) {
    operation.start();
    auto& lease = operation.lease();
    auto result = co_await executeTransactionPool(
        lease.client, lease.slot, std::move(sql), std::move(params), lease.resource, lease.options);
    operation.finishActive();
    co_return result;
}

ScopedOperation<void> DbTransaction::commit() & {
    requireActive();
    return detail::makeScopedOperation(
        operationScope(), commitTask(OperationGuard(state_->operation)));
}

Task<void> DbTransaction::commitTask(OperationGuard operation) {
    operation.start();
    auto& lease = operation.lease();
    co_await commitPoolTransaction(lease.client, lease.slot, lease.resource, lease.options);
    operation.finishClosed();
}

ScopedOperation<void> DbTransaction::rollback() & {
    requireActive();
    return detail::makeScopedOperation(
        operationScope(), rollbackTask(OperationGuard(state_->operation)));
}

Task<void> DbTransaction::rollbackTask(OperationGuard operation) {
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
