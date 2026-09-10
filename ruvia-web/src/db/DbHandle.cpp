#include <utility>

#include "ruvia/web/db/Db.h"
#include "ruvia/web/db/DbQuery.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbPreparedStatement.h"
#include "ruvia/web/detail/db/DbQueryCacheState.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbUtils.h"

// The per-request database handle: it borrows a registry entry for the
// operation's lifetime and hands each call to the pool.

namespace ruvia {

namespace {

Task<DbRows> queryPool(detail::DbPoolRef pool, std::pmr::string sql,
    std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource,
    OperationOptions options) {
    return detail::visitDbPool(pool, [&](auto& client) {
        return client.query(std::move(sql), std::move(params), resource, std::move(options));
    });
}

Task<DbExecResult> executePool(detail::DbPoolRef pool, std::pmr::string sql,
    std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource,
    OperationOptions options) {
    return detail::visitDbPool(pool, [&](auto& client) {
        return client.execute(std::move(sql), std::move(params), resource, std::move(options));
    });
}

Task<DbStreamResult> streamPool(detail::DbPoolRef pool, std::pmr::string sql,
    std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource,
    OperationOptions options) {
    return detail::visitDbPool(pool, [&](auto& client) {
        return client.stream(std::move(sql), std::move(params), resource, std::move(options));
    });
}

Task<DbTransaction> beginPoolTransaction(
    detail::DbPoolRef pool, std::pmr::memory_resource* resource,
    OperationOptions operationOptions, DbTransactionOptions transactionOptions) {
    return detail::visitDbPool(pool,
        [&](auto& client) {
            return client.beginTransaction(
                resource, std::move(operationOptions), std::move(transactionOptions));
        });
}

Task<std::pair<DbRows, DbRows>> queryPairPrepared(detail::DbCacheQuery first,
    detail::DbCacheQuery second, std::optional<std::pmr::string> firstKey,
    std::optional<std::pmr::string> secondKey,
    std::optional<std::chrono::milliseconds> firstDuration,
    std::optional<std::chrono::milliseconds> secondDuration,
    detail::DbQueryCacheState* cache, detail::ScopedOperationScope& scope,
    OperationOptions options) {
    const detail::OperationTimeout operationTimeout(options.timeout);

    auto firstOptions = detail::dbCacheRemainingOptions(options, operationTimeout);
    DbRows rows = cache
                      ? co_await cache->wrap(firstDuration, std::move(firstKey), std::move(first), scope,
                            std::move(firstOptions), operationTimeout)
                      : co_await std::move(first)(std::move(firstOptions));
    if (operationTimeout.expired()) {
        detail::throwDbCacheTimeout();
    }

    auto secondOptions = detail::dbCacheRemainingOptions(options, operationTimeout);
    DbRows count = cache
                       ? co_await cache->wrap(secondDuration, std::move(secondKey), std::move(second), scope,
                             std::move(secondOptions), operationTimeout)
                       : co_await std::move(second)(std::move(secondOptions));
    if (operationTimeout.expired()) {
        detail::throwDbCacheTimeout();
    }
    co_return std::pair{std::move(rows), std::move(count)};
}

}  // namespace

DbHandle::DbHandle(detail::DbPoolRef client, std::pmr::memory_resource* resource,
    detail::ScopedOperationScope& operationScope, detail::DbQueryCacheState* cache) noexcept
    : detail::ScopedCapabilityNode(operationScope, &DbHandle::expireCapability),
      cache_(cache),
      client_(client),
      resource_(detail::pmrResourceOrDefault(resource)) {}

DbHandle::DbHandle(const DbHandle& other) noexcept
    : detail::ScopedCapabilityNode(other),
      cache_(other.cache_),
      client_(other.client_),
      resource_(other.resource_),
      options_(other.options_) {}

DbQueryResultCache DbHandle::queryResultCache() const {
    requireActive();
    if (!cache_) {
        throw DbError(DbError::Code::kNotConfigured, std::nullopt, "database query cache is not configured");
    }
    return DbQueryResultCache(*cache_, operationScope(), options_);
}

DbHandle DbHandle::withOptions(OperationOptions options) const {
    detail::validateOperationOptions(options);
    requireActive();
    DbHandle copy(*this);
    copy.options_ = detail::mergeOperationOptions(options_, std::move(options));
    return copy;
}

void DbHandle::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    auto& handle = static_cast<DbHandle&>(capability);
    handle.cache_ = nullptr;
    handle.client_ = detail::DbPoolRef{};
    handle.resource_ = nullptr;
    handle.options_ = {};
}

DbDriver DbHandle::queryDriver() const {
    requireActive();
    return detail::dbPoolDriver(client_);
}

std::pmr::memory_resource* DbHandle::queryResource() const {
    requireActive();
    return resource_;
}

Task<DbRows> DbHandle::queryTask(const DbQuery& query) const {
    requireActive();
    auto statement = query.compile(queryDriver(), resource_);
    if (!statement.returnsRows()) {
        throw std::invalid_argument("query requires a statement that returns rows");
    }
    const auto driver = queryDriver();
    auto key = cache_ ? cache_->key(query, statement, driver) : std::nullopt;
    if (!cache_ || !key) {
        return queryPool(client_, std::move(statement.sql_), std::move(statement.params_),
            resource_, options_);
    }
    return cache_->wrap(query.cacheDuration(), std::move(key),
        detail::DbCacheQuery(client_, std::nullopt, std::move(statement.sql_),
            std::move(statement.params_), resource_),
        operationScope(), options_);
}

Task<std::pair<DbRows, DbRows>> DbHandle::queryAndCountTask(const DbQuery& query, const DbQuery& count) const {
    requireActive();
    const auto driver = queryDriver();
    auto first = query.compile(driver, resource_);
    auto second = count.compile(driver, resource_);
    if (!first.returnsRows() || !second.returnsRows()) {
        throw std::invalid_argument("query and count require statements that return rows");
    }
    auto firstKey = cache_ ? cache_->key(query, first, driver) : std::nullopt;
    auto secondKey = cache_ ? cache_->key(count, second, driver) : std::nullopt;
    return queryPairPrepared(
        detail::DbCacheQuery(client_, std::nullopt, std::move(first.sql_), std::move(first.params_), resource_),
        detail::DbCacheQuery(client_, std::nullopt, std::move(second.sql_), std::move(second.params_), resource_),
        std::move(firstKey), std::move(secondKey), query.cacheDuration(), count.cacheDuration(), cache_,
        operationScope(), options_);
}

ScopedOperation<DbRows> DbHandle::query(const DbQuery& query) const {
    requireActive();
    return detail::makeScopedOperation(operationScope(), queryTask(query));
}

ScopedOperation<DbExecResult> DbHandle::execute(const DbQuery& query) const {
    requireActive();
    auto statement = query.compile(queryDriver(), resource_);
    if (statement.returnsRows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    return detail::makeScopedOperation(operationScope(),
        executePool(client_, std::move(statement.sql_), std::move(statement.params_), resource_, options_));
}

ScopedOperation<DbRows> DbHandle::query(
    std::string_view sql, std::span<const DbValue> params) const {
    requireActive();
    auto statement = prepareDbStatement(sql, params, resource_);
    return detail::makeScopedOperation(
        operationScope(), queryPool(client_, std::move(statement.sql), std::move(statement.params),
                              resource_, options_));
}

ScopedOperation<DbExecResult> DbHandle::execute(
    std::string_view sql, std::span<const DbValue> params) const {
    requireActive();
    auto statement = prepareDbStatement(sql, params, resource_);
    return detail::makeScopedOperation(
        operationScope(), executePool(client_, std::move(statement.sql),
                              std::move(statement.params), resource_, options_));
}

ScopedOperation<DbStreamResult> DbHandle::queryStream(
    std::string_view sql, std::span<const DbValue> params) const {
    requireActive();
    auto statement = prepareDbStatement(sql, params, resource_);
    return detail::makeScopedOperation(
        operationScope(), queryStreamPrepared(client_, std::move(statement.sql),
                              std::move(statement.params), resource_, operationScope(), options_));
}

Task<DbStreamResult> DbHandle::queryStreamPrepared(detail::DbPoolRef client, std::pmr::string sql,
    std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource,
    detail::ScopedOperationScope& operationScope, OperationOptions options) {
    auto result = co_await streamPool(
        client, std::move(sql), std::move(params), resource, std::move(options));
    result.bindOperationScope(operationScope);
    co_return result;
}

ScopedOperation<DbTransaction> DbHandle::beginTransaction(DbTransactionOptions options) const {
    requireActive();
    return detail::makeScopedOperation(
        operationScope(), beginTransactionPrepared(
                              client_, resource_, operationScope(), options_, std::move(options), cache_));
}

Task<DbTransaction> DbHandle::beginTransactionPrepared(detail::DbPoolRef client,
    std::pmr::memory_resource* resource, detail::ScopedOperationScope& operationScope,
    OperationOptions operationOptions, DbTransactionOptions transactionOptions, detail::DbQueryCacheState* cache) {
    auto transaction = co_await beginPoolTransaction(
        client, resource, std::move(operationOptions), std::move(transactionOptions));
    transaction.bindOperationScope(operationScope);
    transaction.cache_ = cache;
    co_return transaction;
}

}  // namespace ruvia
