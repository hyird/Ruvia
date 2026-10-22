#include <utility>

#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/integration/request_io_metrics.h"

#include "db/db_config_validation.h"
#include "db/db_pool.h"
#include "db/db_pool_utils.h"
#include "db/db_prepared_statement.h"
#include "db/db_query_execution.h"
#ifdef RUVIA_ENABLE_REDIS
#include "db/db_query_cache_state.h"
#endif

// The per-request database handle: it borrows a registry entry for the
// operation's lifetime and hands each call to the pool.

namespace ruvia {

namespace {

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

task<void> discard_rows_task(task<db_rows> rows_task) {
    (void)co_await std::move(rows_task);
}

}  // namespace

db_handle::db_handle(detail::db_pool_ref_type client, std::pmr::memory_resource* resource,
    ::ruvia::operation_scope& operation_scope, detail::db_query_cache_state* cache) noexcept
    : cache_(cache),
      client_(client),
      resource_(pmr_resource_or_default(resource)),
      registration_(operation_scope, this, &db_handle::expire_capability) {}

db_handle::db_handle(const db_handle& other) noexcept
    : cache_(other.cache_),
      client_(other.client_),
      resource_(other.resource_),
      options_(other.options_),
      registration_(other.registration_, this) {}

#ifdef RUVIA_ENABLE_REDIS
db_query_result_cache db_handle::query_result_cache() const {
    registration_.require_active();
    if (!cache_) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "database query cache is not configured");
    }
    return db_query_result_cache(*cache_, registration_.scope(), options_);
}
#endif

db_handle db_handle::with_options(operation_options options) const {
    validate_operation_options(options);
    registration_.require_active();
    db_handle copy(*this);
    copy.options_ = merge_operation_options(options_, std::move(options));
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

detail::db_query_backend db_handle::query_backend() const {
    detail::db_query_backend backend{client_, std::nullopt, resource_, &registration_.scope()};
#ifdef RUVIA_ENABLE_REDIS
    backend.cache_ = cache_;
#endif
    return backend;
}

task<db_rows> db_handle::query_task(const db_query& query) const {
    registration_.require_active();
    const auto backend = query_backend();
    auto plan = detail::db_query_plan::prepare(query, nullptr, backend);
#ifdef RUVIA_ENABLE_REDIS
    if (backend.cache_ != nullptr && plan.first_.cache_key_.has_value()) {
        return detail::execute_db_query_plan<false>(std::move(plan), backend, options_);
    }
#endif
    auto options_copy = options_;
    return detail::query_db_backend(
        backend, std::move(plan.first_.sql_), std::move(plan.first_.params_), options_copy);
}

task<std::pair<db_rows, db_rows>> db_handle::query_and_count_task(const db_query& query, const db_query& count) const {
    registration_.require_active();
    const auto backend = query_backend();
    auto plan = detail::db_query_plan::prepare(query, &count, backend);
    return detail::execute_db_query_plan<true>(std::move(plan), backend, options_);
}

scoped_operation<db_exec_result> db_handle::execute_query(const db_query& query) const {
    registration_.require_active();
    auto statement = query.compile(query_driver(), resource_);
    if (statement.returns_rows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        detail::visit_db_pool(client_, [&](auto& client) {
            return client.execute(std::move(statement.sql_), std::move(statement.params_), resource_, options_, &scope);
        }));
}

scoped_operation<db_rows> db_handle::query(
    std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params, resource_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        detail::visit_db_pool(client_, [&](auto& client) {
            return client.query(std::move(statement.sql_), std::move(statement.params_), resource_, options_, &scope);
        }));
}

scoped_operation<db_exec_result> db_handle::execute(
    std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params, resource_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        detail::visit_db_pool(client_, [&](auto& client) {
            return client.execute(std::move(statement.sql_), std::move(statement.params_), resource_, options_, &scope);
        }));
}

scoped_operation<db_stream_result> db_handle::query_stream(
    std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params, resource_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        query_stream_prepared(client_, std::move(statement.sql_), std::move(statement.params_), resource_, scope, options_));
}

task<db_stream_result> db_handle::query_stream_prepared(detail::db_pool_ref_type client, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    ::ruvia::operation_scope& operation_scope, operation_options options) {
    detail::scoped_request_io_timer timer(operation_scope, detail::request_io_kind::db);
    auto result_value = co_await stream_pool(
        client, std::move(sql), std::move(params), resource, std::move(options));
    result_value.bind_operation_scope(operation_scope);
    co_return result_value;
}

scoped_operation<db_transaction> db_handle::begin_transaction(db_transaction_options options) const {
    registration_.require_active();
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        begin_transaction_prepared(query_backend(), scope, options_, std::move(options)));
}

task<db_transaction> db_handle::begin_transaction_prepared(detail::db_query_backend backend,
    ::ruvia::operation_scope& operation_scope, operation_options operation_options_value,
    db_transaction_options transaction_options) {
    detail::scoped_request_io_timer timer(operation_scope, detail::request_io_kind::db);
    auto transaction = co_await begin_pool_transaction(
        backend.pool_, backend.resource_, std::move(operation_options_value), std::move(transaction_options));
    transaction.bind_operation_scope(operation_scope);
    co_return transaction;
}

scoped_operation<void> db_handle::notify(std::string_view channel, std::string_view payload) const {
    registration_.require_active();
    if (query_driver() != db_driver::postgresql) {
        throw std::invalid_argument("database notify requires PostgreSQL");
    }
    if (channel.empty()) {
        throw std::invalid_argument("database notify channel must not be empty");
    }
    const std::array<db_value, 2> params{db_value(channel), db_value(payload)};
    auto statement = prepare_db_statement("SELECT \"pg_notify\"($1::text, $2::text)", params, resource_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        discard_rows_task(detail::visit_db_pool(client_, [&](auto& client) {
            return client.query(std::move(statement.sql_), std::move(statement.params_), resource_, options_, &scope);
        })));
}

}  // namespace ruvia
