#include <utility>

#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/integration/request_io_metrics.h"

#include "db/db_pool.h"
#include "db/db_pool_utils.h"
#include "db/db_prepared_statement.h"
#include "db/db_query_execution.h"
#include "db/db_sql_format.h"

// An open transaction: it owns a pooled connection until commit, rollback, or
// destruction, and a transaction abandoned by an unwinding scope must roll back
// rather than leak the connection.

namespace ruvia {
namespace {

task<db_rows> query_transaction_pool(detail::db_pool_ref_type pool, std::size_t slot, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    const operation_options& options) {
    return detail::visit_db_pool(pool, [&](auto& client) {
        return client.query_on_transaction_slot(
            slot, std::move(sql), std::move(params), resource, options);
    });
}

task<db_exec_result> execute_transaction_pool(detail::db_pool_ref_type pool, std::size_t slot,
    std::pmr::string sql, std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    const operation_options& options) {
    return detail::visit_db_pool(pool, [&](auto& client) {
        return client.execute_on_transaction_slot(
            slot, std::move(sql), std::move(params), resource, options);
    });
}

task<void> commit_pool_transaction(detail::db_pool_ref_type pool, std::size_t slot,
    std::pmr::memory_resource* resource, const operation_options& options) {
    return detail::visit_db_pool(
        pool, [&](auto& client) { return client.commit_transaction(slot, resource, options); });
}

task<void> rollback_pool_transaction(detail::db_pool_ref_type pool, std::size_t slot,
    std::pmr::memory_resource* resource, const operation_options& options) {
    return detail::visit_db_pool(
        pool, [&](auto& client) { return client.rollback_transaction(slot, resource, options); });
}

void abort_pool_transaction(detail::db_pool_ref_type pool, std::size_t slot) noexcept {
    detail::visit_db_pool_if_present(
        pool, [&](auto& client) noexcept { client.abort_transaction(slot); });
}

}  // namespace

db_transaction::lease_type::lease_type(detail::db_pool_ref_type client, std::size_t slot,
    std::pmr::memory_resource* resource, operation_options options) noexcept
    : client_(client),
      slot_(slot),
      resource_(pmr_resource_or_default(resource)),
      options_(std::move(options)) {}

class db_transaction::state_type final {
public:
    state_type(detail::db_pool_ref_type client, std::size_t slot, std::pmr::memory_resource* resource,
        operation_options options) noexcept
        : operation_(lease_type{client, slot, resource, std::move(options)}) {}

    ~state_type() {
        operation_.reset(
            [](lease_type& lease_value) noexcept { abort_pool_transaction(lease_value.client_, lease_value.slot_); });
    }

    operation_state_type operation_;
};

db_transaction::db_transaction(detail::db_pool_ref_type client, std::size_t slot,
    std::pmr::memory_resource* resource, operation_options options)
    : state_(make_pmr_object<state_type>(resource, client, slot, resource, std::move(options))) {}

db_transaction::db_transaction(db_transaction&& other) noexcept
    : state_(std::move(other.state_)),
      registration_(std::move(other.registration_), this) {}

db_transaction::~db_transaction() = default;

bool db_transaction::active() const noexcept {
    return state_ != nullptr && state_->operation_.active();
}

void db_transaction::bind_operation_scope(::ruvia::operation_scope& scope) noexcept {
    registration_.bind(scope, this, &db_transaction::expire_capability);
}

void db_transaction::expire_capability(void* target) noexcept {
    auto& transaction = *static_cast<db_transaction*>(target);
    transaction.reset();
}

db_driver db_transaction::query_driver() const {
    registration_.require_active();
    return detail::db_pool_driver(state_->operation_.preparation_payload().client_);
}

std::pmr::memory_resource* db_transaction::query_resource() const {
    registration_.require_active();
    return state_->operation_.preparation_payload().resource_;
}

// A transaction never uses the shared query cache: its reads may include its
// own uncommitted writes, which must neither be published nor be replaced by
// rows cached outside the transaction.
detail::db_query_backend db_transaction::query_backend(const lease_type& lease) const {
    return detail::db_query_backend{lease.client_, lease.slot_, lease.resource_};
}

task<db_rows> db_transaction::query_task(const db_query& query) {
    registration_.require_active();
    const auto backend = query_backend(state_->operation_.preparation_payload());
    auto plan = detail::db_query_plan::prepare(query, nullptr, backend);
    operation_guard_type operation(state_->operation_);
    return query_plan_prepared<false>(std::move(plan), backend, std::move(operation));
}

task<std::pair<db_rows, db_rows>> db_transaction::query_and_count_task(const db_query& query, const db_query& count) {
    registration_.require_active();
    const auto backend = query_backend(state_->operation_.preparation_payload());
    auto plan = detail::db_query_plan::prepare(query, &count, backend);
    operation_guard_type operation(state_->operation_);
    return query_plan_prepared<true>(std::move(plan), backend, std::move(operation));
}

template <bool with_count>
task<detail::db_query_result<with_count>> db_transaction::query_plan_prepared(
    detail::db_query_plan plan, detail::db_query_backend backend, operation_guard_type pending) {
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    bool backend_failed = false;
    backend.backend_failed_ = &backend_failed;
    try {
        auto result_value = co_await detail::execute_db_query_plan<with_count>(std::move(plan), backend,
            lease_value.options_);
        operation.finish_active();
        co_return result_value;
    } catch (...) {
        // SQL failures have already retired the lease in the backend. Deadline
        // failures must retire it here while it is still owned by this operation.
        if (!backend_failed) {
            abort_pool_transaction(lease_value.client_, lease_value.slot_);
        }
        throw;
    }
}

scoped_operation<db_exec_result> db_transaction::execute_query(const db_query& query) {
    registration_.require_active();
    const auto& lease_value = state_->operation_.preparation_payload();
    auto statement = query.compile(detail::db_pool_driver(lease_value.client_), lease_value.resource_);
    if (statement.returns_rows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    operation_guard_type operation(state_->operation_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        execute_prepared(std::move(statement.sql_), std::move(statement.params_), scope, std::move(operation)));
}

scoped_operation<db_rows> db_transaction::query(
    std::string_view sql, std::span<const db_value> params) & {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params, state_->operation_.preparation_payload().resource_);
    operation_guard_type operation(state_->operation_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        query_prepared(std::move(statement.sql_), std::move(statement.params_), scope, std::move(operation)));
}

scoped_operation<db_exec_result> db_transaction::execute(
    std::string_view sql, std::span<const db_value> params) & {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params, state_->operation_.preparation_payload().resource_);
    operation_guard_type operation(state_->operation_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        execute_prepared(std::move(statement.sql_), std::move(statement.params_), scope, std::move(operation)));
}

task<db_rows> db_transaction::query_prepared(
    std::pmr::string sql, std::pmr::vector<db_value> params, operation_scope& scope, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(scope, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    auto result_value = co_await query_transaction_pool(
        lease_value.client_, lease_value.slot_, std::move(sql), std::move(params), lease_value.resource_, lease_value.options_);
    operation.finish_active();
    co_return result_value;
}

task<db_exec_result> db_transaction::execute_prepared(
    std::pmr::string sql, std::pmr::vector<db_value> params, operation_scope& scope, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(scope, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    auto result_value = co_await execute_transaction_pool(
        lease_value.client_, lease_value.slot_, std::move(sql), std::move(params), lease_value.resource_, lease_value.options_);
    operation.finish_active();
    co_return result_value;
}

scoped_operation<void> db_transaction::commit() & {
    registration_.require_active();
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(
        scope, commit_task(scope, operation_guard_type(state_->operation_)));
}

task<void> db_transaction::commit_task(operation_scope& scope, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(scope, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    co_await commit_pool_transaction(lease_value.client_, lease_value.slot_, lease_value.resource_, lease_value.options_);
    operation.finish_closed();
}

scoped_operation<void> db_transaction::rollback() & {
    registration_.require_active();
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(
        scope, rollback_task(scope, operation_guard_type(state_->operation_)));
}

task<void> db_transaction::rollback_task(operation_scope& scope, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(scope, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    co_await rollback_pool_transaction(lease_value.client_, lease_value.slot_, lease_value.resource_, lease_value.options_);
    operation.finish_closed();
}
scoped_operation<void> db_transaction::savepoint(std::string_view name) & {
    return run_savepoint_command("SAVEPOINT ", name);
}

scoped_operation<void> db_transaction::rollback_to_savepoint(std::string_view name) & {
    return run_savepoint_command("ROLLBACK TO SAVEPOINT ", name);
}

scoped_operation<void> db_transaction::release_savepoint(std::string_view name) & {
    return run_savepoint_command("RELEASE SAVEPOINT ", name);
}

scoped_operation<void> db_transaction::run_savepoint_command(std::string_view prefix, std::string_view name) {
    registration_.require_active();
    const auto& lease_value = state_->operation_.preparation_payload();
    std::pmr::string sql(prefix, lease_value.resource_);
    detail::append_db_identifier(sql, name, detail::db_pool_driver(lease_value.client_));
    operation_guard_type operation(state_->operation_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(
        scope, savepoint_task(std::move(sql), scope, std::move(operation)));
}

task<void> db_transaction::savepoint_task(
    std::pmr::string sql, operation_scope& scope, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(scope, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    (void)co_await execute_transaction_pool(
        lease_value.client_, lease_value.slot_, std::move(sql), {}, lease_value.resource_, lease_value.options_);
    operation.finish_active();
}

void db_transaction::require_postgresql_transaction(std::string_view feature) const {
    if (query_driver() != db_driver::postgresql) {
        throw std::invalid_argument(std::string(feature) + " requires PostgreSQL");
    }
}

scoped_operation<void> db_transaction::query_void_prepared(std::string_view sql, std::span<const db_value> params) {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params, state_->operation_.preparation_payload().resource_);
    operation_guard_type operation(state_->operation_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        query_void_task(std::move(statement.sql_), std::move(statement.params_), scope, std::move(operation)));
}

scoped_operation<bool> db_transaction::query_bool_prepared(std::string_view sql, std::span<const db_value> params) {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params, state_->operation_.preparation_payload().resource_);
    operation_guard_type operation(state_->operation_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        query_bool_task(std::move(statement.sql_), std::move(statement.params_), scope, std::move(operation)));
}

task<void> db_transaction::query_void_task(
    std::pmr::string sql, std::pmr::vector<db_value> params, operation_scope& scope, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(scope, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    (void)co_await query_transaction_pool(
        lease_value.client_, lease_value.slot_, std::move(sql), std::move(params), lease_value.resource_, lease_value.options_);
    operation.finish_active();
}

task<void> db_transaction::execute_void_task(
    std::pmr::string sql, std::pmr::vector<db_value> params, operation_scope& scope, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(scope, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    (void)co_await execute_transaction_pool(
        lease_value.client_, lease_value.slot_, std::move(sql), std::move(params), lease_value.resource_, lease_value.options_);
    operation.finish_active();
}

task<bool> db_transaction::query_bool_task(
    std::pmr::string sql, std::pmr::vector<db_value> params, operation_scope& scope, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(scope, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    const auto rows = co_await query_transaction_pool(
        lease_value.client_, lease_value.slot_, std::move(sql), std::move(params), lease_value.resource_, lease_value.options_);
    try {
        if (rows.size() != 1) {
            throw std::runtime_error("database boolean scalar query did not return one row");
        }
        const bool result = rows.front()[std::size_t{0}].as<bool>().value();
        operation.finish_active();
        co_return result;
    } catch (...) {
        abort_pool_transaction(lease_value.client_, lease_value.slot_);
        throw;
    }
}

task<std::uint32_t> db_transaction::query_oid_task(
    std::pmr::string sql, std::pmr::vector<db_value> params, operation_scope& scope, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(scope, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    const auto rows = co_await query_transaction_pool(
        lease_value.client_, lease_value.slot_, std::move(sql), std::move(params), lease_value.resource_, lease_value.options_);
    try {
        if (rows.size() != 1) {
            throw std::runtime_error("database OID query did not return one row");
        }
        const auto val = rows.front()[std::size_t{0}].as<std::uint64_t>().value();
        if (val > (std::numeric_limits<std::uint32_t>::max)()) {
            throw std::out_of_range("PostgreSQL OID exceeds 32-bit range");
        }
        operation.finish_active();
        co_return static_cast<std::uint32_t>(val);
    } catch (...) {
        abort_pool_transaction(lease_value.client_, lease_value.slot_);
        throw;
    }
}

task<std::pmr::string> db_transaction::query_text_task(
    std::pmr::string sql, std::pmr::vector<db_value> params, operation_scope& scope, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(scope, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    auto* resource = sql.get_allocator().resource();
    const auto rows = co_await query_transaction_pool(
        lease_value.client_, lease_value.slot_, std::move(sql), std::move(params), lease_value.resource_, lease_value.options_);
    try {
        if (rows.size() != 1) {
            throw std::runtime_error("database text query did not return one row");
        }
        const auto view = rows.front()[std::size_t{0}].as<std::string_view>();
        if (!view) {
            throw db_conversion_error(db_conversion_error::code_type::invalid_format, "database large object text is NULL");
        }
        std::pmr::string result(*view, resource);
        operation.finish_active();
        co_return result;
    } catch (...) {
        abort_pool_transaction(lease_value.client_, lease_value.slot_);
        throw;
    }
}

scoped_operation<void> db_transaction::notify(std::string_view channel, std::string_view payload) & {
    require_postgresql_transaction("database notify");
    if (channel.empty()) {
        throw std::invalid_argument("database notify channel must not be empty");
    }
    const std::array<db_value, 2> params{db_value(channel), db_value(payload)};
    return query_void_prepared("SELECT \"pg_notify\"($1::text, $2::text)", params);
}

scoped_operation<void> db_transaction::advisory_lock(std::int64_t key) & {
    require_postgresql_transaction("advisory_lock");
    const std::array<db_value, 1> params{db_value(key)};
    return query_void_prepared("SELECT \"pg_advisory_xact_lock\"($1::bigint)", params);
}

scoped_operation<void> db_transaction::advisory_lock(std::int32_t class_id, std::int32_t object_id) & {
    require_postgresql_transaction("advisory_lock");
    const std::array<db_value, 2> params{db_value(class_id), db_value(object_id)};
    return query_void_prepared("SELECT \"pg_advisory_xact_lock\"($1::integer, $2::integer)", params);
}

scoped_operation<void> db_transaction::advisory_lock(std::string_view key, std::int64_t seed) & {
    require_postgresql_transaction("advisory_lock");
    const std::array<db_value, 2> params{db_value(key), db_value(seed)};
    return query_void_prepared("SELECT \"pg_advisory_xact_lock\"(\"hashtextextended\"($1::text, $2::bigint))", params);
}

scoped_operation<bool> db_transaction::try_advisory_lock(std::int64_t key) & {
    require_postgresql_transaction("try_advisory_lock");
    const std::array<db_value, 1> params{db_value(key)};
    return query_bool_prepared("SELECT \"pg_try_advisory_xact_lock\"($1::bigint)", params);
}

scoped_operation<bool> db_transaction::try_advisory_lock(std::int32_t class_id, std::int32_t object_id) & {
    require_postgresql_transaction("try_advisory_lock");
    const std::array<db_value, 2> params{db_value(class_id), db_value(object_id)};
    return query_bool_prepared("SELECT \"pg_try_advisory_xact_lock\"($1::integer, $2::integer)", params);
}

scoped_operation<bool> db_transaction::try_advisory_lock(std::string_view key, std::int64_t seed) & {
    require_postgresql_transaction("try_advisory_lock");
    const std::array<db_value, 2> params{db_value(key), db_value(seed)};
    return query_bool_prepared("SELECT \"pg_try_advisory_xact_lock\"(\"hashtextextended\"($1::text, $2::bigint))", params);
}

scoped_operation<void> db_transaction::declare_cursor(
    std::string_view name, std::string_view sql, std::span<const db_value> params, bool scroll) & {
    require_postgresql_transaction("declare_cursor");
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    const auto& lease_value = state_->operation_.preparation_payload();
    auto statement = prepare_db_statement(sql, params, lease_value.resource_);
    std::pmr::string command("DECLARE ", lease_value.resource_);
    detail::append_db_identifier(command, name, db_driver::postgresql);
    command += scroll ? " SCROLL CURSOR FOR " : " NO SCROLL CURSOR FOR ";
    command += statement.sql_;
    operation_guard_type operation(state_->operation_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        execute_void_task(std::move(command), std::move(statement.params_), scope, std::move(operation)));
}

scoped_operation<db_rows> db_transaction::fetch_cursor(std::string_view name, std::uint64_t count) & {
    require_postgresql_transaction("fetch_cursor");
    if (count > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        throw std::invalid_argument("cursor fetch count exceeds signed 64-bit range");
    }
    const auto& lease_value = state_->operation_.preparation_payload();
    std::pmr::string sql("FETCH FORWARD ", lease_value.resource_);
    detail::append_db_number(sql, count);
    sql += " FROM ";
    detail::append_db_identifier(sql, name, db_driver::postgresql);
    operation_guard_type operation(state_->operation_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        query_prepared(std::move(sql), {}, scope, std::move(operation)));
}

scoped_operation<void> db_transaction::close_cursor(std::string_view name) & {
    require_postgresql_transaction("close_cursor");
    return run_savepoint_command("CLOSE ", name);
}

scoped_operation<std::uint32_t> db_transaction::lo_create(std::uint32_t oid) & {
    require_postgresql_transaction("lo_create");
    const std::array<db_value, 1> params{db_value(static_cast<std::uint64_t>(oid))};
    auto statement = prepare_db_statement("SELECT \"lo_create\"($1::oid)::bigint", params,
        state_->operation_.preparation_payload().resource_);
    operation_guard_type operation(state_->operation_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        query_oid_task(std::move(statement.sql_), std::move(statement.params_), scope, std::move(operation)));
}

scoped_operation<void> db_transaction::lo_put(std::uint32_t oid, std::int64_t offset, std::string_view utf8_bytes) & {
    require_postgresql_transaction("lo_put");
    if (offset < 0) {
        throw std::invalid_argument("large object offset must be non-negative");
    }
    const std::array<db_value, 3> params{db_value(static_cast<std::uint64_t>(oid)), db_value(offset), db_value(utf8_bytes)};
    return query_void_prepared("SELECT \"lo_put\"($1::oid, $2::bigint, \"convert_to\"($3::text, 'UTF8'))", params);
}

scoped_operation<std::pmr::string> db_transaction::lo_get_text(std::uint32_t oid, std::string_view encoding) & {
    require_postgresql_transaction("lo_get_text");
    if (encoding.empty()) {
        throw std::invalid_argument("large object encoding must not be empty");
    }
    const std::array<db_value, 2> params{db_value(static_cast<std::uint64_t>(oid)), db_value(encoding)};
    auto statement = prepare_db_statement("SELECT \"convert_from\"(\"lo_get\"($1::oid), $2::text)", params,
        state_->operation_.preparation_payload().resource_);
    operation_guard_type operation(state_->operation_);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        query_text_task(std::move(statement.sql_), std::move(statement.params_), scope, std::move(operation)));
}

scoped_operation<bool> db_transaction::lo_unlink(std::uint32_t oid) & {
    require_postgresql_transaction("lo_unlink");
    const std::array<db_value, 1> params{db_value(static_cast<std::uint64_t>(oid))};
    return query_bool_prepared("SELECT (\"lo_unlink\"($1::oid) = 1)", params);
}

void db_transaction::reset() noexcept {
    if (state_ != nullptr) {
        state_->operation_.reset(
            [](lease_type& lease_value) noexcept { abort_pool_transaction(lease_value.client_, lease_value.slot_); });
    }
}

}  // namespace ruvia
