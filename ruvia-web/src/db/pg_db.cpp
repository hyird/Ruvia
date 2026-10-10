#include <libpq-fe.h>

#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_postgresql.h"
#include "db/db_registry.h"

namespace ruvia::detail {
namespace {

void free_postgresql_result(void* result_value) noexcept {
    PQclear(static_cast<PGresult*>(result_value));
}

class postgresql_result_owner final {
public:
    explicit postgresql_result_owner(PGresult* result_value) noexcept
        : result_(result_value) {}

    ~postgresql_result_owner() {
        reset();
    }

    postgresql_result_owner(const postgresql_result_owner&) = delete;
    postgresql_result_owner& operator=(const postgresql_result_owner&) = delete;

    [[nodiscard]] PGresult* get() const noexcept {
        return result_;
    }

    [[nodiscard]] PGresult& operator*() const noexcept {
        return *result_;
    }

    [[nodiscard]] PGresult* release() noexcept {
        return std::exchange(result_, nullptr);
    }

    void reset() noexcept {
        if (result_ != nullptr) {
            PQclear(result_);
            result_ = nullptr;
        }
    }

private:
    PGresult* result_;
};

// Every caller closes the connection after a failed statement, so the rest of
// the command is never drained here: PQgetResult() blocks the worker until the
// server's next message arrives, and the server flushes ErrorResponse before
// the ReadyForQuery that ends the command.
[[noreturn]] void throw_postgresql_statement_error(PGconn& connection, std::string_view operation, postgresql_result_owner& result_value) {
    auto error = postgresql_error(connection, operation, db_error::code_type::statement_failed, result_value.get());
    result_value.reset();
    throw error;
}

[[nodiscard]] bool successful_result_status(ExecStatusType status) noexcept {
    return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK || status == PGRES_EMPTY_QUERY;
}

void materialize_borrowed_result(
    db_rows& output, PGresult& result_value, std::pmr::memory_resource* resource) {
    const auto row_count = static_cast<std::size_t>(PQntuples(&result_value));
    const auto field_count = static_cast<std::size_t>(PQnfields(&result_value));
    auto& rows = db_result_access::rows(output);
    auto& fields_value = db_result_access::fields(output);
    auto& column_names = db_result_access::column_names(output);
    rows.reserve(row_count);
    fields_value.reserve(row_count * field_count);
    column_names.reserve(field_count);
    for (std::size_t field = 0; field < field_count; ++field) {
        column_names.emplace_back(PQfname(&result_value, static_cast<int>(field)));
    }
    for (std::size_t row = 0; row < row_count; ++row) {
        const auto row_start = fields_value.size();
        for (std::size_t field = 0; field < field_count; ++field) {
            const auto row_index = static_cast<int>(row);
            const auto field_index = static_cast<int>(field);
            if (PQgetisnull(&result_value, row_index, field_index) != 0) {
                fields_value.push_back(db_result_access::null_field(resource));
                continue;
            }
            fields_value.push_back(db_result_access::borrowed_field(
                std::string_view(PQgetvalue(&result_value, row_index, field_index),
                    static_cast<std::size_t>(PQgetlength(&result_value, row_index, field_index))),
                resource));
        }
        rows.push_back(db_result_access::borrowed_row(fields_value.data() + row_start, field_count,
            column_names.data(), column_names.size(), resource));
    }
}

[[nodiscard]] db_row materialize_owned_single_row(
    PGresult& result_value, std::pmr::memory_resource* resource) {
    auto row = db_result_access::owned_row(resource);
    auto& fields_value = db_result_access::owned_fields(row);
    auto& column_names = db_result_access::owned_column_names(row);
    const auto field_count = static_cast<std::size_t>(PQnfields(&result_value));
    fields_value.reserve(field_count);
    column_names.reserve(field_count);
    for (std::size_t field = 0; field < field_count; ++field) {
        const auto field_index = static_cast<int>(field);
        column_names.emplace_back(PQfname(&result_value, field_index));
        if (PQgetisnull(&result_value, 0, field_index) != 0) {
            fields_value.push_back(db_result_access::null_field(resource));
            continue;
        }
        fields_value.push_back(db_result_access::owned_field(
            std::string_view(PQgetvalue(&result_value, 0, field_index),
                static_cast<std::size_t>(PQgetlength(&result_value, 0, field_index))),
            resource));
    }
    return row;
}

}  // namespace

task<db_rows> postgresql_pool::query(std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options) {
    return execute_db_query(*this, std::move(sql), std::move(params), resource, std::move(options));
}

task<db_exec_result> postgresql_pool::execute(std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options) {
    return execute_db_command(*this, std::move(sql), std::move(params), resource, std::move(options));
}

task<db_rows> postgresql_pool::query_on_slot(connection_slot_type& slot, const std::pmr::string& sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource,
    const ruvia::operation_timeout& operation_timeout_value) {
    throw_if_cancelled(slot);
    if (!slot.connected_) {
        co_await connect_unlocked(slot, operation_timeout_value);
    }
    const ruvia::operation_timeout deadline_value = operation_timeout_value.constrained_by(config_.query_timeout_);
    co_await send_query(slot, sql, params, deadline_value, false);

    auto output = db_result_access::make_result(resource);
    bool retained_tuple_result = false;
    while (true) {
        co_await wait_until_result_ready(slot, deadline_value);
        postgresql_result_owner result_value(PQgetResult(slot.connection_));
        if (result_value.get() == nullptr) {
            break;
        }
        const auto status = PQresultStatus(result_value.get());
        if (!successful_result_status(status)) {
            throw_postgresql_statement_error(*slot.connection_, "PostgreSQL query", result_value);
        }

        if (status == PGRES_TUPLES_OK) {
            if (retained_tuple_result) {
                result_value.reset();
                throw db_error(db_error::code_type::protocol_error, db_driver::postgresql,
                    "PostgreSQL returned multiple tuple results");
            }
            materialize_borrowed_result(output, *result_value, resource);
            db_result_access::own_raw_result(output, result_value.release(), &free_postgresql_result);
            retained_tuple_result = true;
        }
    }
    if (!retained_tuple_result) {
        throw std::invalid_argument("query() requires row-producing SQL");
    }
    co_return output;
}

task<db_exec_result> postgresql_pool::execute_on_slot(connection_slot_type& slot, const std::pmr::string& sql,
    std::span<const db_value> params, std::pmr::memory_resource*,
    const ruvia::operation_timeout& operation_timeout_value) {
    throw_if_cancelled(slot);
    if (!slot.connected_) {
        co_await connect_unlocked(slot, operation_timeout_value);
    }
    const ruvia::operation_timeout deadline_value = operation_timeout_value.constrained_by(config_.query_timeout_);
    co_await send_query(slot, sql, params, deadline_value, false);

    std::uint64_t affected_rows = 0;
    while (true) {
        co_await wait_until_result_ready(slot, deadline_value);
        postgresql_result_owner result_value(PQgetResult(slot.connection_));
        if (result_value.get() == nullptr) {
            break;
        }
        const auto status = PQresultStatus(result_value.get());
        if (!successful_result_status(status)) {
            throw_postgresql_statement_error(*slot.connection_, "PostgreSQL execute", result_value);
        }
        if (status == PGRES_TUPLES_OK) {
            result_value.reset();
            throw std::invalid_argument("execute() does not accept row-producing SQL");
        }
        affected_rows = postgresql_affected_rows(*result_value);
    }
    co_return db_result_access::make_exec_result(affected_rows);
}

task<db_stream_result> postgresql_pool::stream(std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    const ruvia::operation_timeout operation_timeout_value(options.timeout_);
    const auto slot_index = co_await acquire_slot(operation_timeout_value, options.stop_token_);
    db_slot_cancellation_guard cancellation(*this, slot_index, options.stop_token_);
    try {
        auto& slot = slots_[slot_index];
        throw_if_cancelled(slot);
        if (!slot.connected_) {
            co_await connect_unlocked(slot, operation_timeout_value);
        }
        const ruvia::operation_timeout deadline_value = operation_timeout_value.constrained_by(config_.query_timeout_);
        co_await send_query(slot, sql, std::span<const db_value>(params), deadline_value, true);
        co_return db_stream_result(db_pool_ref_type{this}, slot_index, nullptr, resource, std::move(options));
    } catch (...) {
        close_slot(slots_[slot_index]);
        cancellation.finish();
        release_slot(slot_index);
        throw;
    }
}

task<std::optional<db_row>> postgresql_pool::read_stream_row(std::size_t slot_index, void*,
    std::pmr::memory_resource* resource, const operation_options& options) {
    if (slot_index >= slots_.size()) {
        co_return std::nullopt;
    }
    auto& slot = slots_[slot_index];
    db_slot_cancellation_guard cancellation(*this, slot_index, options.stop_token_);
    bool slot_released = false;
    try {
        throw_if_cancelled(slot);
        const ruvia::operation_timeout deadline_value =
            ruvia::operation_timeout(options.timeout_).constrained_by(config_.query_timeout_);
        co_await wait_until_result_ready(slot, deadline_value);
        postgresql_result_owner result_value(PQgetResult(slot.connection_));
        if (result_value.get() == nullptr) {
            slot_released = true;
            cancellation.finish();
            release_slot(slot_index);
            co_return std::nullopt;
        }
        const auto status = PQresultStatus(result_value.get());
        if (status == PGRES_SINGLE_TUPLE) {
            auto row = materialize_owned_single_row(*result_value, resource);
            co_return row;
        }
        if (status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK ||
            status == PGRES_EMPTY_QUERY) {
            result_value.reset();
            while (true) {
                co_await wait_until_result_ready(slot, deadline_value);
                postgresql_result_owner remaining(PQgetResult(slot.connection_));
                if (remaining.get() == nullptr) {
                    break;
                }
                const auto remaining_status = PQresultStatus(remaining.get());
                if (!successful_result_status(remaining_status)) {
                    auto error = postgresql_error(*slot.connection_, "PostgreSQL stream",
                        db_error::code_type::statement_failed, remaining.get());
                    throw error;
                }
            }
            slot_released = true;
            cancellation.finish();
            release_slot(slot_index);
            if (status == PGRES_COMMAND_OK || status == PGRES_EMPTY_QUERY) {
                throw std::invalid_argument("query_stream() requires row-producing SQL");
            }
            co_return std::nullopt;
        }
        auto error = postgresql_error(
            *slot.connection_, "PostgreSQL stream", db_error::code_type::statement_failed, result_value.get());
        throw error;
    } catch (...) {
        if (!slot_released) {
            close_slot(slot);
            cancellation.finish();
            release_slot(slot_index);
        }
        throw;
    }
}

task<void> postgresql_pool::close_stream(
    std::size_t slot, void*, std::pmr::memory_resource*, const operation_options& options) {
    if (slot < slots_.size()) {
        db_slot_cancellation_guard cancellation(*this, slot, options.stop_token_);
        // Abandoning a libpq single-row result still requires draining the whole
        // command. Closing is bounded and keeps the worker non-blocking.
        close_slot(slots_[slot]);
        cancellation.finish();
        release_slot(slot);
        if (options.stop_token_.stop_requested()) {
            throw db_error(
                db_error::code_type::cancelled, db_driver::postgresql, "database operation cancelled");
        }
    }
    co_return;
}

void postgresql_pool::abort_stream(std::size_t slot, void*) noexcept {
    if (slot >= slots_.size()) {
        return;
    }
    close_slot(slots_[slot]);
    release_slot(slot);
}

task<void> postgresql_pool::execute_control(connection_slot_type& slot, std::string_view sql,
    std::pmr::memory_resource* resource, const ruvia::operation_timeout& operation_timeout_value) {
    const std::pmr::string command(sql, resource_);
    (void)co_await execute_on_slot(slot, command, {}, resource, operation_timeout_value);
}

task<db_rows> postgresql_pool::query_on_transaction_slot(std::size_t slot, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    const operation_options& options) {
    return query_on_db_transaction_slot(
        *this, slot, std::move(sql), std::move(params), resource, options);
}

task<db_exec_result> postgresql_pool::execute_on_transaction_slot(std::size_t slot, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    const operation_options& options) {
    return execute_on_db_transaction_slot(
        *this, slot, std::move(sql), std::move(params), resource, options);
}

task<db_transaction> postgresql_pool::begin_transaction(
    std::pmr::memory_resource* resource, operation_options operation_options_value,
    db_transaction_options transaction_options) {
    return begin_db_transaction(*this, resource,
        std::move(operation_options_value), make_db_transaction_start_plan(db_driver::postgresql, transaction_options));
}

task<void> postgresql_pool::commit_transaction(
    std::size_t slot, std::pmr::memory_resource* resource, const operation_options& options) {
    return finish_db_transaction(*this, slot, "COMMIT", resource, options);
}

task<void> postgresql_pool::rollback_transaction(
    std::size_t slot, std::pmr::memory_resource* resource, const operation_options& options) {
    return finish_db_transaction(*this, slot, "ROLLBACK", resource, options);
}

void postgresql_pool::abort_transaction(std::size_t slot) noexcept {
    if (slot >= slots_.size()) {
        return;
    }
    close_slot(slots_[slot]);
    release_slot(slot);
}

}  // namespace ruvia::detail
