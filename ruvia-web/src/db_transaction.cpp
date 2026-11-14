#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "connection_lifecycle.h"
#include "db_pool.h"
#include "db_query.h"
#include "db_query_plan.h"
#include "db_query_storage.h"
#include "db_session.h"
#include "db_sql_dialect.h"
#include "operation_options.h"

// An open transaction: it owns a pooled connection until commit, rollback, or
// destruction, and a transaction abandoned by an unwinding scope must roll back
// rather than leak the connection.

namespace ruvia {

db_transaction::lease_type::lease_type(detail::db_pool_ref_type client, std::size_t slot, operation_options options) noexcept :
    client_(client),
    slot_(slot),
    options_(std::move(options)) {}

void db_transaction::lease_type::abort() noexcept {
    detail::visit_db_pool_if_present(client_, [&](auto& client) noexcept {
        client.abort_transaction(slot_);
    });
}

db_transaction::db_transaction(detail::db_pool_ref_type client, std::size_t slot, operation_options options) :
    state_(std::make_unique<operation_state_type>(lease_type{client, slot, std::move(options)})) {}

db_transaction::db_transaction(db_transaction&& other) noexcept :
    state_(std::move(other.state_)),
    io_metrics_(other.io_metrics_),
    registration_(std::move(other.registration_), this) {}

db_transaction::~db_transaction() = default;

bool db_transaction::active() const noexcept {
    return state_ != nullptr && state_->active();
}

void db_transaction::bind_operation_scope(::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics) noexcept {
    io_metrics_ = io_metrics;
    registration_.bind(scope, this, &db_transaction::expire_capability);
}

void db_transaction::expire_capability(void* target) noexcept {
    auto& transaction = *static_cast<db_transaction*>(target);
    transaction.reset();
}

db_driver db_transaction::query_driver() const {
    registration_.require_active();
    return detail::db_pool_driver(state_->preparation_payload().client_);
}

// A transaction never uses the shared query cache: its reads may include its
// own uncommitted writes, which must neither be published nor be replaced by
// rows cached outside the transaction.
detail::db_query_backend db_transaction::query_backend(const lease_type& lease) const {
    return detail::db_query_backend{lease.client_, lease.slot_, io_metrics_};
}

task<db_rows> db_transaction::query_task(const db_query& query) {
    registration_.require_active();
    const auto backend = query_backend(state_->preparation_payload());
    auto plan = detail::db_query_plan::prepare(query, nullptr, backend);
    operation_guard_type operation(*state_);
    return query_plan_prepared<false>(std::move(plan), backend, std::move(operation));
}

task<std::pair<db_rows, db_rows>> db_transaction::query_and_count_task(const db_query& query, const db_query& count) {
    registration_.require_active();
    const auto backend = query_backend(state_->preparation_payload());
    auto plan = detail::db_query_plan::prepare(query, &count, backend);
    operation_guard_type operation(*state_);
    return query_plan_prepared<true>(std::move(plan), backend, std::move(operation));
}

template <bool with_count>
task<detail::db_query_result<with_count>> db_transaction::query_plan_prepared(detail::db_query_plan plan, detail::db_query_backend backend, operation_guard_type pending) {
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    bool backend_failed = false;
    backend.backend_failed_ = &backend_failed;
    try {
        auto result_value = co_await detail::execute_db_query_plan<with_count>(std::move(plan), backend, lease_value.options_);
        operation.finish_active();
        co_return result_value;
    } catch (...) {
        // SQL failures have already retired the lease in the backend. Deadline
        // failures must retire it here while it is still owned by this operation.
        if (!backend_failed) {
            lease_value.abort();
        }
        throw;
    }
}

template <typename result_type, bool exec_statement>
task<result_type> db_transaction::run_statement(std::string sql, std::vector<db_value> params) {
    operation_guard_type operation(*state_);
    return ::ruvia::make_scoped_operation(registration_.scope(), statement_task<result_type, exec_statement>(std::move(sql), std::move(params), io_metrics_, std::move(operation)));
}

template <typename result_type, bool exec_statement>
task<result_type> db_transaction::statement_task(std::string sql, std::vector<db_value> params, detail::request_io_metrics* io_metrics, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    if constexpr (exec_statement) {
        auto result_value = co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
            return client.execute_on_transaction_slot(lease_value.slot_, std::move(sql), std::move(params), lease_value.options_);
        });
        operation.finish_active();
        if constexpr (!std::is_void_v<result_type>) {
            co_return result_value;
        }
    } else {
        auto rows = co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
            return client.query_on_transaction_slot(lease_value.slot_, std::move(sql), std::move(params), lease_value.options_);
        });
        if constexpr (std::is_same_v<result_type, bool>) {
            try {
                if (rows.size() != 1) {
                    throw std::runtime_error("database boolean scalar query did not return one row");
                }
                const bool result = rows.front()[std::size_t{0}].template as<bool>().value();
                operation.finish_active();
                co_return result;
            } catch (...) {
                lease_value.abort();
                throw;
            }
        } else {
            operation.finish_active();
            if constexpr (!std::is_void_v<result_type>) {
                co_return rows;
            }
        }
    }
}

template <bool commit_lease>
task<void> db_transaction::finish_task(detail::request_io_metrics* io_metrics, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
        return commit_lease ? client.commit_transaction(lease_value.slot_, lease_value.options_) : client.rollback_transaction(lease_value.slot_, lease_value.options_);
    });
    operation.finish_closed();
}

task<db_exec_result> db_transaction::execute_query(const db_query& query) {
    registration_.require_active();
    const auto& lease_value = state_->preparation_payload();
    auto statement = query.compile(detail::db_pool_driver(lease_value.client_));
    if (statement.returns_rows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    return run_statement<db_exec_result, true>(std::move(statement.sql_), std::move(statement.params_));
}

task<db_exec_result> db_transaction::execute_prepared(std::string sql) {
    registration_.require_active();
    return run_statement<db_exec_result, true>(std::move(sql), std::vector<db_value>{});
}

task<db_rows> db_transaction::query(std::string_view sql, std::span<const db_value> params) & {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return run_statement<db_rows, false>(std::move(statement.sql_), std::move(statement.params_));
}

task<db_exec_result> db_transaction::execute(std::string_view sql, std::span<const db_value> params) & {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return run_statement<db_exec_result, true>(std::move(statement.sql_), std::move(statement.params_));
}

task<void> db_transaction::commit() & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(), finish_task<true>(io_metrics_, operation_guard_type(*state_)));
}

task<void> db_transaction::rollback() & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(), finish_task<false>(io_metrics_, operation_guard_type(*state_)));
}

task<void> db_transaction::savepoint(std::string_view name) & {
    return run_identifier_command("SAVEPOINT ", name);
}

task<void> db_transaction::rollback_to_savepoint(std::string_view name) & {
    return run_identifier_command("ROLLBACK TO SAVEPOINT ", name);
}

task<void> db_transaction::release_savepoint(std::string_view name) & {
    return run_identifier_command("RELEASE SAVEPOINT ", name);
}

task<void> db_transaction::run_identifier_command(std::string_view prefix, std::string_view name) {
    registration_.require_active();
    const auto& lease_value = state_->preparation_payload();
    const auto driver = detail::db_pool_driver(lease_value.client_);
    detail::validate_db_identifier(name, driver);
    std::string sql;
    sql.reserve(prefix.size() + name.size() + 2);
    sql.append(prefix);
    detail::append_db_identifier(sql, name, driver);
    return run_statement<void, true>(std::move(sql), std::vector<db_value>{});
}

void db_transaction::require_postgresql_transaction(std::string_view feature) const {
    if (query_driver() != db_driver::postgresql) {
        throw std::invalid_argument(std::string(feature) + " requires PostgreSQL");
    }
}

task<void> db_transaction::query_void_prepared(std::string_view sql, std::span<const db_value> params) {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return run_statement<void, false>(std::move(statement.sql_), std::move(statement.params_));
}

task<bool> db_transaction::query_bool_prepared(std::string_view sql, std::span<const db_value> params) {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return run_statement<bool, false>(std::move(statement.sql_), std::move(statement.params_));
}

task<void> db_transaction::notify(std::string_view channel, std::string_view payload) & {
    require_postgresql_transaction("database notify");
    auto statement = prepare_db_notify(channel, payload);
    return run_statement<void, false>(std::move(statement.sql_), std::move(statement.params_));
}

task<void> db_transaction::advisory_lock(std::int64_t key) & {
    require_postgresql_transaction("advisory_lock");
    const std::array<db_value, 1> params{db_value(key)};
    return query_void_prepared("SELECT \"pg_advisory_xact_lock\"($1::bigint)", params);
}

task<void> db_transaction::advisory_lock(std::int32_t class_id, std::int32_t object_id) & {
    require_postgresql_transaction("advisory_lock");
    const std::array<db_value, 2> params{db_value(class_id), db_value(object_id)};
    return query_void_prepared("SELECT \"pg_advisory_xact_lock\"($1::integer, $2::integer)", params);
}

task<void> db_transaction::advisory_lock(std::string_view key, std::int64_t seed) & {
    require_postgresql_transaction("advisory_lock");
    const std::array<db_value, 2> params{db_value(key), db_value(seed)};
    return query_void_prepared("SELECT \"pg_advisory_xact_lock\"(\"hashtextextended\"($1::text, $2::bigint))", params);
}

task<bool> db_transaction::try_advisory_lock(std::int64_t key) & {
    require_postgresql_transaction("try_advisory_lock");
    const std::array<db_value, 1> params{db_value(key)};
    return query_bool_prepared("SELECT \"pg_try_advisory_xact_lock\"($1::bigint)", params);
}

task<bool> db_transaction::try_advisory_lock(std::int32_t class_id, std::int32_t object_id) & {
    require_postgresql_transaction("try_advisory_lock");
    const std::array<db_value, 2> params{db_value(class_id), db_value(object_id)};
    return query_bool_prepared("SELECT \"pg_try_advisory_xact_lock\"($1::integer, $2::integer)", params);
}

task<bool> db_transaction::try_advisory_lock(std::string_view key, std::int64_t seed) & {
    require_postgresql_transaction("try_advisory_lock");
    const std::array<db_value, 2> params{db_value(key), db_value(seed)};
    return query_bool_prepared("SELECT \"pg_try_advisory_xact_lock\"(\"hashtextextended\"($1::text, $2::bigint))", params);
}

task<void> db_transaction::declare_cursor(std::string_view name, std::string_view sql, std::span<const db_value> params, bool scroll) & {
    require_postgresql_transaction("declare_cursor");
    validate_db_sql_text(sql);
    detail::validate_db_identifier(name, db_driver::postgresql);
    const std::string_view mode = scroll ? " SCROLL CURSOR FOR " : " NO SCROLL CURSOR FOR ";
    std::string command;
    command.reserve(8 + name.size() + 2 + mode.size() + sql.size());
    command.append("DECLARE ");
    detail::append_db_identifier(command, name, db_driver::postgresql);
    command.append(mode);
    command.append(sql);
    auto owned_params = detail::clone_db_values(params);
    return run_statement<void, true>(std::move(command), std::move(owned_params));
}

task<db_rows> db_transaction::fetch_cursor(std::string_view name, std::uint64_t count) & {
    require_postgresql_transaction("fetch_cursor");
    if (count > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        throw std::invalid_argument("cursor fetch count exceeds signed 64-bit range");
    }
    detail::validate_db_identifier(name, db_driver::postgresql);
    std::string sql;
    sql.reserve(14 + 20 + 6 + name.size() + 2);
    sql.append("FETCH FORWARD ");
    detail::append_db_number(sql, count);
    sql.append(" FROM ");
    detail::append_db_identifier(sql, name, db_driver::postgresql);
    return run_statement<db_rows, false>(std::move(sql), std::vector<db_value>{});
}

task<void> db_transaction::close_cursor(std::string_view name) & {
    require_postgresql_transaction("close_cursor");
    return run_identifier_command("CLOSE ", name);
}

void db_transaction::reset() noexcept {
    if (state_ != nullptr) {
        state_->reset();
    }
}

// A streaming query result: the rows arrive one at a time from a pooled
// connection the result holds for as long as it is active, and every exit --
// exhaustion, explicit close, failure, or destruction -- must return that
// connection exactly once.

db_stream_result::lease_type::lease_type(detail::db_pool_ref_type client, std::size_t slot, operation_options options) noexcept :
    client_(client),
    slot_(slot),
    options_(std::move(options)) {}

void db_stream_result::lease_type::abort() noexcept {
    detail::visit_db_pool_if_present(client_, [&](auto& client) noexcept {
        client.abort_stream(slot_);
    });
}

db_stream_result::state_type::state_type(lease_type lease, std::vector<std::string> column_names) noexcept :
    column_names_(std::move(column_names)),
    operation_(std::move(lease)) {}

db_stream_result::db_stream_result(detail::db_pool_ref_type client, std::size_t slot, std::vector<std::string> column_names, operation_options options) :
    state_(std::make_unique<state_type>(lease_type{client, slot, std::move(options)}, std::move(column_names))) {}

db_stream_result::db_stream_result(db_stream_result&& other) noexcept :
    state_(std::move(other.state_)),
    io_metrics_(other.io_metrics_),
    registration_(std::move(other.registration_), this) {}

db_stream_result::~db_stream_result() = default;

bool db_stream_result::active() const noexcept {
    return state_ != nullptr && state_->operation_.active();
}

std::span<const std::string> db_stream_result::column_names() const& {
    registration_.require_active();
    return state_ != nullptr ? std::span<const std::string>(state_->column_names_) : std::span<const std::string>{};
}

void db_stream_result::bind_operation_scope(::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics) noexcept {
    io_metrics_ = io_metrics;
    registration_.bind(scope, this, &db_stream_result::expire_capability);
}

void db_stream_result::expire_capability(void* target) noexcept {
    auto& result_value = *static_cast<db_stream_result*>(target);
    result_value.reset();
}

namespace {

// The stream's handle options with this operation's own: its timeout only
// tightens, a supplied token replaces the inherited one. An already-stopped
// token gives the lease back before it reaches the connection, so the stream
// stays usable.
template <typename guard_type>
[[nodiscard]] operation_options admit_stream_operation(guard_type& operation, operation_options operation_value) {
    auto effective = override_operation_options(operation.lease().options_, std::move(operation_value));
    if (effective.stop_token_.stop_requested()) {
        operation.finish_active();
        throw db_error(db_error::code_type::cancelled, std::nullopt, "database stream operation cancelled");
    }
    return effective;
}

}  // namespace

task<std::optional<db_row>> db_stream_result::read(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(), read_task(io_metrics_, operation_guard_type(state_->operation_), state_->column_names_, std::move(operation)));
}

task<std::optional<db_row>> db_stream_result::read_task(detail::request_io_metrics* io_metrics, operation_guard_type pending, std::vector<std::string>& column_names, operation_options operation_value) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto effective = admit_stream_operation(operation, std::move(operation_value));
    auto& lease_value = operation.lease();
    auto row = co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
        return client.read_stream_row(lease_value.slot_, lease_value.fields_, column_names, effective);
    });
    if (row) {
        operation.finish_active();
    } else {
        operation.finish_closed();
    }
    co_return row;
}

task<void> db_stream_result::close(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(), close_task(io_metrics_, operation_guard_type(state_->operation_), std::move(operation)));
}

task<void> db_stream_result::close_task(detail::request_io_metrics* io_metrics, operation_guard_type pending, operation_options operation_value) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    if (operation.finished()) {
        co_return;
    }
    operation.start();
    auto effective = admit_stream_operation(operation, std::move(operation_value));
    auto& lease_value = operation.lease();
    co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
        return client.close_stream(lease_value.slot_, effective);
    });
    operation.finish_closed();
}

void db_stream_result::reset() noexcept {
    if (state_ != nullptr) {
        state_->operation_.reset();
    }
}

}  // namespace ruvia
