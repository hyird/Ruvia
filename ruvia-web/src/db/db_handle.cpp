#include <utility>

#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_config_validation.h"
#include "db/db_prepared_statement.h"
#include "db/db_query_cache_state.h"
#include "db/db_registry.h"

// The per-request database handle: it borrows a registry entry for the
// operation's lifetime and hands each call to the pool.

namespace ruvia {

namespace {

task<db_rows> query_pool(detail::db_pool_ref_type pool, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    operation_options options) {
    return detail::visit_db_pool(pool, [&](auto& client) {
        return client.query(std::move(sql), std::move(params), resource, std::move(options));
    });
}

task<db_exec_result> execute_pool(detail::db_pool_ref_type pool, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    operation_options options) {
    return detail::visit_db_pool(pool, [&](auto& client) {
        return client.execute(std::move(sql), std::move(params), resource, std::move(options));
    });
}

task<db_stream_result> stream_pool(detail::db_pool_ref_type pool, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    operation_options options) {
    return detail::visit_db_pool(pool, [&](auto& client) {
        return client.stream(std::move(sql), std::move(params), resource, std::move(options));
    });
}

task<db_transaction> begin_pool_transaction(
    detail::db_pool_ref_type pool, std::pmr::memory_resource* resource,
    operation_options operation_options_value, db_transaction_options transaction_options) {
    return detail::visit_db_pool(pool,
        [&](auto& client) {
            return client.begin_transaction(
                resource, std::move(operation_options_value), std::move(transaction_options));
        });
}

}  // namespace

db_handle::db_handle(detail::db_pool_ref_type client, std::pmr::memory_resource* resource,
    ::ruvia::operation_scope& operation_scope, detail::db_query_cache_state* cache) noexcept
    : cache_(cache),
      client_(client),
      resource_(detail::pmr_resource_or_default(resource)),
      registration_(operation_scope, this, &db_handle::expire_capability) {}

db_handle::db_handle(const db_handle& other) noexcept
    : cache_(other.cache_),
      client_(other.client_),
      resource_(other.resource_),
      options_(other.options_),
      registration_(other.registration_, this) {}

db_query_result_cache db_handle::query_result_cache() const {
    registration_.require_active();
    if (!cache_) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "database query cache is not configured");
    }
    return db_query_result_cache(*cache_, registration_.scope(), options_);
}

db_handle db_handle::with_options(operation_options options) const {
    detail::validate_operation_options(options);
    registration_.require_active();
    db_handle copy(*this);
    copy.options_ = detail::merge_operation_options(options_, std::move(options));
    return copy;
}

void db_handle::expire_capability(void* target) noexcept {
    auto& handle = *static_cast<db_handle*>(target);
    handle.cache_ = nullptr;
    handle.client_ = detail::db_pool_ref_type{};
    handle.resource_ = nullptr;
    handle.options_ = {};
}

db_driver db_handle::query_driver() const {
    registration_.require_active();
    return detail::db_pool_driver(client_);
}

std::pmr::memory_resource* db_handle::query_resource() const {
    registration_.require_active();
    return resource_;
}

task<db_rows> db_handle::query_task(const db_query& query) const {
    registration_.require_active();
    auto plan = detail::db_query_plan::prepare(query, nullptr, query_driver(), resource_, cache_);
    return detail::execute_db_query_plan<false>(std::move(plan),
        detail::db_query_backend{client_, std::nullopt, resource_, cache_}, options_);
}

task<std::pair<db_rows, db_rows>> db_handle::query_and_count_task(const db_query& query, const db_query& count) const {
    registration_.require_active();
    auto plan = detail::db_query_plan::prepare(query, &count, query_driver(), resource_, cache_);
    return detail::execute_db_query_plan<true>(std::move(plan),
        detail::db_query_backend{client_, std::nullopt, resource_, cache_}, options_);
}

scoped_operation<db_rows> db_handle::query(const db_query& query) const {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(), query_task(query));
}

scoped_operation<db_exec_result> db_handle::execute(const db_query& query) const {
    registration_.require_active();
    auto statement = query.compile(query_driver(), resource_);
    if (statement.returns_rows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    return ::ruvia::make_scoped_operation(registration_.scope(),
        execute_pool(client_, std::move(statement.sql_), std::move(statement.params_), resource_, options_));
}

scoped_operation<db_rows> db_handle::query(
    std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params, resource_);
    return ::ruvia::make_scoped_operation(
        registration_.scope(), query_pool(client_, std::move(statement.sql_), std::move(statement.params_),
                                   resource_, options_));
}

scoped_operation<db_exec_result> db_handle::execute(
    std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params, resource_);
    return ::ruvia::make_scoped_operation(
        registration_.scope(), execute_pool(client_, std::move(statement.sql_),
                                   std::move(statement.params_), resource_, options_));
}

scoped_operation<db_stream_result> db_handle::query_stream(
    std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params, resource_);
    return ::ruvia::make_scoped_operation(
        registration_.scope(), query_stream_prepared(client_, std::move(statement.sql_),
                                   std::move(statement.params_), resource_, registration_.scope(), options_));
}

task<db_stream_result> db_handle::query_stream_prepared(detail::db_pool_ref_type client, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    ::ruvia::operation_scope& operation_scope, operation_options options) {
    auto result_value = co_await stream_pool(
        client, std::move(sql), std::move(params), resource, std::move(options));
    result_value.bind_operation_scope(operation_scope);
    co_return result_value;
}

scoped_operation<db_transaction> db_handle::begin_transaction(db_transaction_options options) const {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), begin_transaction_prepared(
                                   client_, resource_, registration_.scope(), options_, std::move(options), cache_));
}

task<db_transaction> db_handle::begin_transaction_prepared(detail::db_pool_ref_type client,
    std::pmr::memory_resource* resource, ::ruvia::operation_scope& operation_scope,
    operation_options operation_options_value, db_transaction_options transaction_options, detail::db_query_cache_state* cache) {
    auto transaction = co_await begin_pool_transaction(
        client, resource, std::move(operation_options_value), std::move(transaction_options));
    transaction.bind_operation_scope(operation_scope);
    transaction.cache_ = cache;
    co_return transaction;
}

}  // namespace ruvia
