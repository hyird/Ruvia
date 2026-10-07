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

}  // namespace

DbHandle::DbHandle(detail::DbPoolRef client, std::pmr::memory_resource* resource,
    ::ruvia::operation_scope& operationScope, detail::DbQueryCacheState* cache) noexcept
    : cache_(cache),
      client_(client),
      resource_(detail::pmrResourceOrDefault(resource)),
      registration_(operationScope, this, &DbHandle::expire_capability) {}

DbHandle::DbHandle(const DbHandle& other) noexcept
    : cache_(other.cache_),
      client_(other.client_),
      resource_(other.resource_),
      options_(other.options_),
      registration_(other.registration_, this) {}

DbQueryResultCache DbHandle::queryResultCache() const {
    registration_.require_active();
    if (!cache_) {
        throw DbError(DbError::Code::kNotConfigured, std::nullopt, "database query cache is not configured");
    }
    return DbQueryResultCache(*cache_, registration_.scope(), options_);
}

DbHandle DbHandle::withOptions(OperationOptions options) const {
    detail::validateOperationOptions(options);
    registration_.require_active();
    DbHandle copy(*this);
    copy.options_ = detail::mergeOperationOptions(options_, std::move(options));
    return copy;
}

void DbHandle::expire_capability(void* target) noexcept {
    auto& handle = *static_cast<DbHandle*>(target);
    handle.cache_ = nullptr;
    handle.client_ = detail::DbPoolRef{};
    handle.resource_ = nullptr;
    handle.options_ = {};
}

DbDriver DbHandle::queryDriver() const {
    registration_.require_active();
    return detail::dbPoolDriver(client_);
}

std::pmr::memory_resource* DbHandle::queryResource() const {
    registration_.require_active();
    return resource_;
}

Task<DbRows> DbHandle::queryTask(const DbQuery& query) const {
    registration_.require_active();
    auto plan = detail::db_query_plan::prepare(query, nullptr, queryDriver(), resource_, cache_);
    return detail::execute_db_query_plan<false>(std::move(plan),
        detail::db_query_backend{client_, std::nullopt, resource_, cache_}, options_);
}

Task<std::pair<DbRows, DbRows>> DbHandle::queryAndCountTask(const DbQuery& query, const DbQuery& count) const {
    registration_.require_active();
    auto plan = detail::db_query_plan::prepare(query, &count, queryDriver(), resource_, cache_);
    return detail::execute_db_query_plan<true>(std::move(plan),
        detail::db_query_backend{client_, std::nullopt, resource_, cache_}, options_);
}

ScopedOperation<DbRows> DbHandle::query(const DbQuery& query) const {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(), queryTask(query));
}

ScopedOperation<DbExecResult> DbHandle::execute(const DbQuery& query) const {
    registration_.require_active();
    auto statement = query.compile(queryDriver(), resource_);
    if (statement.returnsRows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    return ::ruvia::make_scoped_operation(registration_.scope(),
        executePool(client_, std::move(statement.sql_), std::move(statement.params_), resource_, options_));
}

ScopedOperation<DbRows> DbHandle::query(
    std::string_view sql, std::span<const DbValue> params) const {
    registration_.require_active();
    auto statement = prepareDbStatement(sql, params, resource_);
    return ::ruvia::make_scoped_operation(
        registration_.scope(), queryPool(client_, std::move(statement.sql), std::move(statement.params),
                                   resource_, options_));
}

ScopedOperation<DbExecResult> DbHandle::execute(
    std::string_view sql, std::span<const DbValue> params) const {
    registration_.require_active();
    auto statement = prepareDbStatement(sql, params, resource_);
    return ::ruvia::make_scoped_operation(
        registration_.scope(), executePool(client_, std::move(statement.sql),
                                   std::move(statement.params), resource_, options_));
}

ScopedOperation<DbStreamResult> DbHandle::queryStream(
    std::string_view sql, std::span<const DbValue> params) const {
    registration_.require_active();
    auto statement = prepareDbStatement(sql, params, resource_);
    return ::ruvia::make_scoped_operation(
        registration_.scope(), queryStreamPrepared(client_, std::move(statement.sql),
                                   std::move(statement.params), resource_, registration_.scope(), options_));
}

Task<DbStreamResult> DbHandle::queryStreamPrepared(detail::DbPoolRef client, std::pmr::string sql,
    std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource,
    ::ruvia::operation_scope& operationScope, OperationOptions options) {
    auto result = co_await streamPool(
        client, std::move(sql), std::move(params), resource, std::move(options));
    result.bindOperationScope(operationScope);
    co_return result;
}

ScopedOperation<DbTransaction> DbHandle::beginTransaction(DbTransactionOptions options) const {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), beginTransactionPrepared(
                                   client_, resource_, registration_.scope(), options_, std::move(options), cache_));
}

Task<DbTransaction> DbHandle::beginTransactionPrepared(detail::DbPoolRef client,
    std::pmr::memory_resource* resource, ::ruvia::operation_scope& operationScope,
    OperationOptions operationOptions, DbTransactionOptions transactionOptions, detail::DbQueryCacheState* cache) {
    auto transaction = co_await beginPoolTransaction(
        client, resource, std::move(operationOptions), std::move(transactionOptions));
    transaction.bindOperationScope(operationScope);
    transaction.cache_ = cache;
    co_return transaction;
}

}  // namespace ruvia
