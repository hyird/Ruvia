#include <utility>

#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_pool.h"
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
    operation_guard_type operation(state_->operation_);
    const auto backend = query_backend(state_->operation_.preparation_payload());
    auto plan = detail::db_query_plan::prepare(query, nullptr, backend);
    return query_plan_prepared<false>(std::move(plan), backend, std::move(operation));
}

task<std::pair<db_rows, db_rows>> db_transaction::query_and_count_task(const db_query& query, const db_query& count) {
    registration_.require_active();
    operation_guard_type operation(state_->operation_);
    const auto backend = query_backend(state_->operation_.preparation_payload());
    auto plan = detail::db_query_plan::prepare(query, &count, backend);
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

scoped_operation<db_rows> db_transaction::query(const db_query& query) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(), query_task(query));
}

scoped_operation<db_exec_result> db_transaction::execute(const db_query& query) & {
    registration_.require_active();
    operation_guard_type operation(state_->operation_);
    const auto& lease_value = state_->operation_.preparation_payload();
    auto statement = query.compile(detail::db_pool_driver(lease_value.client_), lease_value.resource_);
    if (statement.returns_rows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    return ::ruvia::make_scoped_operation(registration_.scope(),
        execute_prepared(std::move(statement.sql_), std::move(statement.params_), std::move(operation)));
}

scoped_operation<db_rows> db_transaction::query(
    std::string_view sql, std::span<const db_value> params) & {
    registration_.require_active();
    operation_guard_type operation(state_->operation_);
    auto statement = prepare_db_statement(sql, params, state_->operation_.preparation_payload().resource_);
    return ::ruvia::make_scoped_operation(registration_.scope(),
        query_prepared(std::move(statement.sql_), std::move(statement.params_), std::move(operation)));
}

scoped_operation<db_exec_result> db_transaction::execute(
    std::string_view sql, std::span<const db_value> params) & {
    registration_.require_active();
    operation_guard_type operation(state_->operation_);
    auto statement = prepare_db_statement(sql, params, state_->operation_.preparation_payload().resource_);
    return ::ruvia::make_scoped_operation(
        registration_.scope(), execute_prepared(std::move(statement.sql_), std::move(statement.params_),
                                   std::move(operation)));
}

task<db_rows> db_transaction::query_prepared(
    std::pmr::string sql, std::pmr::vector<db_value> params, operation_guard_type pending) {
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    auto result_value = co_await query_transaction_pool(
        lease_value.client_, lease_value.slot_, std::move(sql), std::move(params), lease_value.resource_, lease_value.options_);
    operation.finish_active();
    co_return result_value;
}

task<db_exec_result> db_transaction::execute_prepared(
    std::pmr::string sql, std::pmr::vector<db_value> params, operation_guard_type pending) {
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
    return ::ruvia::make_scoped_operation(
        registration_.scope(), commit_task(operation_guard_type(state_->operation_)));
}

task<void> db_transaction::commit_task(operation_guard_type pending) {
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    co_await commit_pool_transaction(lease_value.client_, lease_value.slot_, lease_value.resource_, lease_value.options_);
    operation.finish_closed();
}

scoped_operation<void> db_transaction::rollback() & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), rollback_task(operation_guard_type(state_->operation_)));
}

task<void> db_transaction::rollback_task(operation_guard_type pending) {
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
    operation_guard_type operation(state_->operation_);
    const auto& lease_value = state_->operation_.preparation_payload();
    std::pmr::string sql(prefix, lease_value.resource_);
    detail::append_db_identifier(sql, name, detail::db_pool_driver(lease_value.client_));
    return ::ruvia::make_scoped_operation(
        registration_.scope(), savepoint_task(std::move(sql), std::move(operation)));
}

task<void> db_transaction::savepoint_task(std::pmr::string sql, operation_guard_type pending) {
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    (void)co_await execute_transaction_pool(
        lease_value.client_, lease_value.slot_, std::move(sql), {}, lease_value.resource_, lease_value.options_);
    operation.finish_active();
}

void db_transaction::reset() noexcept {
    if (state_ != nullptr) {
        state_->operation_.reset(
            [](lease_type& lease_value) noexcept { abort_pool_transaction(lease_value.client_, lease_value.slot_); });
    }
}

}  // namespace ruvia
