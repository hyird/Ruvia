// The per-request database handle: it borrows a registry entry for the
// operation's lifetime and hands each call to the pool.

#include "db_executor.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "connection_lifecycle.h"
#include "db_pool.h"
#include "db_query.h"
#include "db_query_plan.h"
#include "db_query_storage.h"
#include "db_sql_scan.h"
#include "io_operation.h"
#include "operation_options.h"
#ifdef RUVIA_ENABLE_REDIS
#include "db_query_cache.h"
#endif

namespace ruvia {

namespace {

task<void> discard_rows_task(task<db_rows> rows_task) {
    (void)co_await std::move(rows_task);
}

// One statement on the pool with the handle's options and the request's
// cancellation linked. Owns its inputs: the link may start it later.
template <bool exec_statement>
[[nodiscard]] auto start_db_statement(const operation_options& options, const stop_token& request_stop, detail::db_pool_ref_type client, std::string sql, std::vector<db_value> params, detail::request_io_metrics* io_metrics) {
    return detail::start_request_linked(options, request_stop, [client, sql = std::move(sql), params = std::move(params), io_metrics](const operation_options& linked) mutable {
        return detail::visit_db_pool(client, [&](auto& pool) {
            if constexpr (exec_statement) {
                return pool.execute(std::move(sql), std::move(params), linked, io_metrics);
            } else {
                return pool.query(std::move(sql), std::move(params), linked, io_metrics);
            }
        });
    });
}

[[nodiscard]] constexpr bool has_sql_non_whitespace(std::string_view sql) noexcept {
    for (const auto character : sql) {
        if (!detail::is_sql_whitespace(character)) {
            return true;
        }
    }
    return false;
}

}  // namespace

db_executor::db_executor(detail::db_pool_ref_type client, ::ruvia::operation_scope& operation_scope, detail::request_io_metrics* io_metrics, detail::db_query_cache_state* cache) noexcept :
    cache_(cache),
    client_(client),
    io_metrics_(io_metrics),
    registration_(operation_scope, this, &db_executor::expire_capability) {}

db_executor::db_executor(const db_executor& other) noexcept :
    cache_(other.cache_),
    client_(other.client_),
    io_metrics_(other.io_metrics_),
    options_(other.options_),
    request_stop_(other.request_stop_),
    registration_(other.registration_, this) {}

#ifdef RUVIA_ENABLE_REDIS
db_query_result_cache db_executor::query_result_cache() const {
    registration_.require_active();
    if (!cache_) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "database query cache is not configured");
    }
    return db_query_result_cache(*cache_, registration_.scope(), io_metrics_, options_, request_stop_);
}
#endif

db_executor db_executor::with_options(operation_options options) const {
    registration_.require_active();
    db_executor copy(*this);
    copy.options_ = override_operation_options(options_, std::move(options));
    return copy;
}

const worker_ref& db_executor::worker() const& {
    registration_.require_active();
    return detail::visit_db_pool(client_, [](const auto& client) -> const worker_ref& {
        return client.worker();
    });
}

void db_executor::expire_capability(void* target) noexcept {
    auto& handle = *static_cast<db_executor*>(target);
    handle.cache_ = nullptr;
    handle.client_ = detail::db_pool_ref_type{};
    handle.io_metrics_ = nullptr;
    handle.options_ = {};
    handle.request_stop_ = {};
}

db_driver db_executor::query_driver() const {
    registration_.require_active();
    return detail::db_pool_driver(client_);
}

detail::db_query_backend db_executor::query_backend() const {
    detail::db_query_backend backend{client_, std::nullopt, io_metrics_};
#ifdef RUVIA_ENABLE_REDIS
    backend.cache_ = cache_;
#endif
    return backend;
}

task<db_rows> db_executor::query_task(const db_query& query) const {
    registration_.require_active();
    const auto backend = query_backend();
    auto plan = detail::db_query_plan::prepare(query, nullptr, backend);
#ifdef RUVIA_ENABLE_REDIS
    if (backend.cache_ != nullptr && plan.first_.cache_key_.has_value()) {
        return detail::start_request_linked(options_, request_stop_, [plan = std::move(plan), backend](const operation_options& options) mutable {
            return detail::execute_db_query_plan<false>(std::move(plan), backend, options);
        });
    }
#endif
    return detail::start_request_linked(options_, request_stop_, [backend, sql = std::move(plan.first_.sql_), params = std::move(plan.first_.params_)](const operation_options& options) mutable {
        auto options_copy = options;
        return detail::query_db_backend(backend, std::move(sql), std::move(params), options_copy);
    });
}

task<std::pair<db_rows, db_rows>> db_executor::query_and_count_task(const db_query& query, const db_query& count) const {
    registration_.require_active();
    const auto backend = query_backend();
    auto plan = detail::db_query_plan::prepare(query, &count, backend);
    return detail::start_request_linked(options_, request_stop_, [plan = std::move(plan), backend](const operation_options& options) mutable {
        return detail::execute_db_query_plan<true>(std::move(plan), backend, options);
    });
}

task<db_exec_result> db_executor::execute_query(const db_query& query) const {
    registration_.require_active();
    auto statement = query.compile(query_driver());
    if (statement.returns_rows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    return ::ruvia::make_scoped_operation(registration_.scope(), start_db_statement<true>(options_, request_stop_, client_, std::move(statement.sql_), std::move(statement.params_), io_metrics_));
}

task<db_exec_result> db_executor::execute_prepared(std::string sql) const {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(), start_db_statement<true>(options_, request_stop_, client_, std::move(sql), {}, io_metrics_));
}

task<db_rows> db_executor::query(std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return ::ruvia::make_scoped_operation(registration_.scope(), start_db_statement<false>(options_, request_stop_, client_, std::move(statement.sql_), std::move(statement.params_), io_metrics_));
}

task<db_exec_result> db_executor::execute(std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return ::ruvia::make_scoped_operation(registration_.scope(), start_db_statement<true>(options_, request_stop_, client_, std::move(statement.sql_), std::move(statement.params_), io_metrics_));
}

task<db_stream_result> db_executor::query_stream(std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope, query_stream_prepared(client_, std::move(statement.sql_), std::move(statement.params_), scope, io_metrics_, options_, request_stop_));
}

task<db_stream_result> db_executor::query_stream_prepared(detail::db_pool_ref_type client, std::string sql, std::vector<db_value> params, ::ruvia::operation_scope& operation_scope, detail::request_io_metrics* io_metrics, operation_options options, stop_token request_stop) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    // The stream outlives this call, so it owns the link its reads use.
    auto request_link = detail::make_request_link(options, request_stop);
    auto stream_options = request_link ? request_link->options() : detail::request_bound_options(options, request_stop);
    auto result_value = co_await detail::visit_db_pool(client, [&](auto& pool) {
        return pool.stream(std::move(sql), std::move(params), std::move(stream_options));
    });
    result_value.bind_operation_scope(operation_scope, io_metrics, std::move(request_link));
    co_return result_value;
}

task<db_transaction> db_executor::begin_transaction(db_transaction_options options) const {
    registration_.require_active();
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope, begin_transaction_prepared(query_backend(), scope, options_, request_stop_, std::move(options)));
}

task<db_transaction> db_executor::begin_transaction_prepared(detail::db_query_backend backend, ::ruvia::operation_scope& operation_scope, operation_options operation_options_value, stop_token request_stop, db_transaction_options transaction_options) {
    detail::scoped_request_io_timer timer(backend.io_metrics_, detail::request_io_kind::db);
    // The transaction outlives this call, so it owns the link its statements use.
    auto request_link = detail::make_request_link(operation_options_value, request_stop);
    auto transaction_operation_options = request_link ? request_link->options() : detail::request_bound_options(operation_options_value, request_stop);
    auto transaction = co_await detail::visit_db_pool(backend.pool_, [&](auto& pool) {
        return pool.begin_transaction(std::move(transaction_operation_options), std::move(transaction_options));
    });
    transaction.bind_operation_scope(operation_scope, backend.io_metrics_, std::move(request_link));
    co_return transaction;
}

task<void> db_executor::notify(std::string_view channel, std::string_view payload) const {
    registration_.require_active();
    if (query_driver() != db_driver::postgresql) {
        throw std::invalid_argument("database notify requires PostgreSQL");
    }
    auto statement = prepare_db_notify(channel, payload);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope, discard_rows_task(start_db_statement<false>(options_, request_stop_, client_, std::move(statement.sql_), std::move(statement.params_), io_metrics_)));
}

db_executor db_executor::bind_caller_scope(::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics, const stop_token& stop_token_value) && {
    registration_.require_active();
    db_executor bound(client_, scope, io_metrics, cache_);
    bound.options_ = options_;
    bound.request_stop_ = stop_token_value;
    return bound;
}

}  // namespace ruvia

namespace ruvia {

void validate_db_sql_text(std::string_view sql) {
    if (!has_sql_non_whitespace(sql)) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if (sql.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SQL must not contain NUL bytes");
    }
}

prepared_db_statement prepare_db_statement(std::string_view sql, std::span<const db_value> params) {
    validate_db_sql_text(sql);
    return prepared_db_statement{std::string(sql), detail::clone_db_values(params)};
}

prepared_db_statement prepare_db_notify(std::string_view channel, std::string_view payload) {
    if (channel.empty()) {
        throw std::invalid_argument("database notify channel must not be empty");
    }
    const std::array<db_value, 2> params{db_value(channel), db_value(payload)};
    return prepare_db_statement("SELECT \"pg_notify\"($1::text, $2::text)", params);
}

namespace detail {
namespace {

[[noreturn]] void throw_db_plan_timeout(db_driver driver) {
    throw db_error(db_error::code_type::timeout, driver, "database query timed out");
}

}  // namespace

db_query_plan db_query_plan::prepare(const db_query& query, const db_query* count, const db_query_backend& backend) {
    const auto driver = db_pool_driver(backend.pool_);
    const auto prepare_step = [&](const db_query& input) {
        auto statement = input.compile(driver);
        if (!statement.returns_rows()) {
            throw std::invalid_argument(count ? "query and count require statements that return rows" : "query requires a statement that returns rows");
        }
#ifdef RUVIA_ENABLE_REDIS
        std::optional<std::string> key;
        if (backend.cache_ != nullptr) {
            key = backend.cache_->key(input, statement, driver);
        }
        return db_query_step{std::move(statement.sql_), std::move(statement.params_), std::move(key), input.cache_duration()};
#else
        return db_query_step{std::move(statement.sql_), std::move(statement.params_)};
#endif
    };
    db_query_plan plan{prepare_step(query), std::nullopt};
    if (count != nullptr) {
        plan.second_.emplace(prepare_step(*count));
    }
    return plan;
}

task<db_rows> query_db_backend(const db_query_backend& backend, std::string sql, std::vector<db_value> params, operation_options& options) {
    return visit_db_pool(backend.pool_, [&](auto& pool) {
        if (backend.slot_.has_value()) {
            return pool.query_on_transaction_slot(*backend.slot_, std::move(sql), std::move(params), options);
        }
        return pool.query(std::move(sql), std::move(params), std::move(options), backend.io_metrics_);
    });
}

// One deadline and one execution chain for direct, cached, and mixed sequences.
// A statement without a cache key goes straight to the pool.
template <bool with_count>
task<db_query_result<with_count>> execute_db_query_plan(db_query_plan plan, db_query_backend backend, operation_options options) {
    const auto driver = db_pool_driver(backend.pool_);
    const operation_deadline deadline_value(options.timeout_);
    std::array<std::optional<db_rows>, with_count ? 2 : 1> results;
    for (std::size_t index = 0; index < results.size(); ++index) {
        auto& step = index == 0 ? plan.first_ : plan.second_.value();
        if (deadline_value.expired()) {
            throw_db_plan_timeout(driver);
        }
        auto step_options = db_remaining_options(options, deadline_value);
#ifdef RUVIA_ENABLE_REDIS
        if (backend.cache_ != nullptr && step.cache_key_.has_value()) {
            // The cache runs only on pooled backends; a transaction never
            // carries one.
            results[index].emplace(co_await backend.cache_->wrap(step.cache_duration_, std::move(*step.cache_key_), db_cache_query(backend, std::move(step.sql_), std::move(step.params_)), std::move(step_options), deadline_value));
        } else
#endif
        {
            scoped_request_io_timer timer(backend.slot_.has_value() ? backend.io_metrics_ : nullptr, request_io_kind::db);
            try {
                results[index].emplace(co_await query_db_backend(backend, std::move(step.sql_), std::move(step.params_), step_options));
            } catch (...) {
                if (backend.backend_failed_ != nullptr) {
                    *backend.backend_failed_ = true;
                }
                throw;
            }
        }
        if (deadline_value.expired()) {
            throw_db_plan_timeout(driver);
        }
    }
    if constexpr (with_count) {
        co_return std::pair{std::move(*results[0]), std::move(*results[1])};
    } else {
        co_return std::move(*results[0]);
    }
}

template task<db_query_result<false>> execute_db_query_plan<false>(db_query_plan plan, db_query_backend backend, operation_options options);
template task<db_query_result<true>> execute_db_query_plan<true>(db_query_plan plan, db_query_backend backend, operation_options options);

}  // namespace detail

}  // namespace ruvia
