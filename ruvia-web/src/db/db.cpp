#include <mysql.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_registry.h"
#include "db/db_sql.h"

namespace ruvia {

task<db_rows> detail::mariadb_pool::query(std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options) {
    return execute_db_query(*this, std::move(sql), std::move(params), resource, std::move(options));
}

task<db_exec_result> detail::mariadb_pool::execute(std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    operation_options options) {
    return execute_db_command(*this, std::move(sql), std::move(params), resource, std::move(options));
}

task<db_stream_result> detail::mariadb_pool::stream(std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    operation_options options) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }

    const ruvia::operation_timeout operation_timeout_value(options.timeout_);
    const auto slot_index = co_await acquire_slot(operation_timeout_value, options.stop_token_);
    db_slot_cancellation_guard cancellation(*this, slot_index, options.stop_token_);
    bool slot_released = false;
    try {
        auto& slot = slots_[slot_index];
        co_await run_mysql_statement(
            slot, sql, std::span<const db_value>(params), resource, operation_timeout_value);
        auto* raw_result = mysql_use_result(slot.connection_);
        if (raw_result == nullptr) {
            if (mysql_field_count(slot.connection_) != 0) {
                throw mysql_error(
                    *slot.connection_, "mysql_use_result", db_error::code_type::statement_failed);
            }
            slot_released = true;
            cancellation.finish();
            release_slot(slot_index);
            throw std::invalid_argument("query_stream() requires row-producing SQL");
        }

        co_return db_stream_result(
            db_pool_ref_type{this}, slot_index, raw_result, resource, std::move(options));
    } catch (...) {
        if (!slot_released) {
            close_slot(slots_[slot_index]);
            cancellation.finish();
            release_slot(slot_index);
        }
        throw;
    }
}

task<std::optional<db_row>> detail::mariadb_pool::read_stream_row(std::size_t slot, void* result_value,
    std::pmr::memory_resource* resource, const operation_options& options) {
    if (slot >= slots_.size() || result_value == nullptr) {
        co_return std::nullopt;
    }

    db_slot_cancellation_guard cancellation(*this, slot, options.stop_token_);
    auto* raw_result = static_cast<MYSQL_RES*>(result_value);
    try {
        throw_if_cancelled(slots_[slot]);
        const ruvia::operation_timeout deadline_value =
            ruvia::operation_timeout(options.timeout_).constrained_by(config_.query_timeout_);
        MYSQL_ROW row = nullptr;
        int status = mysql_fetch_row_start(&row, raw_result);
        while (status != 0) {
            status = mysql_fetch_row_cont(
                &row, raw_result, co_await wait_for_mysql(slots_[slot], status, deadline_value));
        }

        if (row != nullptr) {
            const auto field_count = static_cast<std::size_t>(mysql_num_fields(raw_result));
            const auto* lengths = mysql_fetch_lengths(raw_result);
            auto output_row = db_result_access::owned_row(resource);
            auto& output_fields = db_result_access::owned_fields(output_row);
            auto& output_column_names = db_result_access::owned_column_names(output_row);
            output_fields.reserve(field_count);
            output_column_names.reserve(field_count);
            const auto* fields_value = mysql_fetch_fields(raw_result);
            for (std::size_t i = 0; i < field_count; ++i) {
                output_column_names.emplace_back(
                    fields_value[i].name, static_cast<std::size_t>(fields_value[i].name_length));
                if (row[i] == nullptr) {
                    output_fields.push_back(db_result_access::null_field(resource));
                    continue;
                }
                output_fields.push_back(
                    db_result_access::owned_field(std::string_view(row[i], lengths[i]), resource));
            }
            co_return output_row;
        }
    } catch (...) {
        close_slot(slots_[slot]);
        cancellation.finish();
        release_slot(slot);
        throw;
    }

    // EOF is not a read failure. The close path owns slot release, including
    // its own failure path, so it must run outside the read-error guard.
    cancellation.finish();
    co_await close_stream(slot, result_value, resource, options);
    co_return std::nullopt;
}

task<void> detail::mariadb_pool::close_stream(
    std::size_t slot, void* result_value, std::pmr::memory_resource*, const operation_options& options) {
    if (slot >= slots_.size() || result_value == nullptr) {
        co_return;
    }

    db_slot_cancellation_guard cancellation(*this, slot, options.stop_token_);
    auto* raw_result = static_cast<MYSQL_RES*>(result_value);
    try {
        throw_if_cancelled(slots_[slot]);
        const ruvia::operation_timeout deadline_value =
            ruvia::operation_timeout(options.timeout_).constrained_by(config_.query_timeout_);
        int status = mysql_free_result_start(raw_result);
        while (status != 0) {
            status = mysql_free_result_cont(
                raw_result, co_await wait_for_mysql(slots_[slot], status, deadline_value));
        }
        cancellation.finish();
        release_slot(slot);
    } catch (...) {
        close_slot(slots_[slot]);
        cancellation.finish();
        release_slot(slot);
        throw;
    }
}

void detail::mariadb_pool::abort_stream(std::size_t slot, void*) noexcept {
    if (slot >= slots_.size()) {
        return;
    }
    close_slot(slots_[slot]);
    release_slot(slot);
}

task<db_rows> detail::mariadb_pool::query_on_transaction_slot(std::size_t slot, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    const operation_options& options) {
    return detail::query_on_db_transaction_slot(
        *this, slot, std::move(sql), std::move(params), resource, options);
}

task<db_exec_result> detail::mariadb_pool::execute_on_transaction_slot(std::size_t slot,
    std::pmr::string sql, std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    const operation_options& options) {
    return detail::execute_on_db_transaction_slot(
        *this, slot, std::move(sql), std::move(params), resource, options);
}

task<db_rows> detail::mariadb_pool::query_on_slot(connection_slot_type& slot, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource,
    const ruvia::operation_timeout& operation_timeout_value) {
    const auto deadline_value = co_await run_mysql_statement(slot, sql, params, resource, operation_timeout_value);
    auto& connection = *slot.connection_;

    auto result_value = db_result_access::make_result(resource);
    auto* raw_result = co_await store_mysql_result(slot, deadline_value);
    if (raw_result == nullptr) {
        if (mysql_field_count(&connection) != 0) {
            throw mysql_error(connection, "mysql_store_result", db_error::code_type::statement_failed);
        }
        throw std::invalid_argument("query() requires row-producing SQL");
    }

    db_result_access::own_raw_result(result_value, raw_result, &free_stored_result);
    const auto field_count = static_cast<std::size_t>(mysql_num_fields(raw_result));
    const auto row_count = static_cast<std::size_t>(mysql_num_rows(raw_result));
    auto& result_rows = db_result_access::rows(result_value);
    auto& result_fields = db_result_access::fields(result_value);
    auto& result_column_names = db_result_access::column_names(result_value);
    result_rows.reserve(row_count);
    result_fields.reserve(row_count * field_count);
    result_column_names.reserve(field_count);
    const auto* fields_value = mysql_fetch_fields(raw_result);
    for (std::size_t i = 0; i < field_count; ++i) {
        result_column_names.emplace_back(
            fields_value[i].name, static_cast<std::size_t>(fields_value[i].name_length));
    }
    while (auto* row = mysql_fetch_row(raw_result)) {
        const auto* lengths = mysql_fetch_lengths(raw_result);
        const auto row_start = result_fields.size();
        for (std::size_t i = 0; i < field_count; ++i) {
            if (row[i] == nullptr) {
                result_fields.push_back(db_result_access::null_field(resource));
                continue;
            }
            result_fields.push_back(
                db_result_access::borrowed_field(std::string_view(row[i], lengths[i]), resource));
        }
        result_rows.push_back(db_result_access::borrowed_row(result_fields.data() + row_start, field_count,
            result_column_names.data(), result_column_names.size(), resource));
    }

    co_return result_value;
}

task<db_exec_result> detail::mariadb_pool::execute_on_slot(connection_slot_type& slot, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource,
    const ruvia::operation_timeout& operation_timeout_value) {
    const auto deadline_value = co_await run_mysql_statement(slot, sql, params, resource, operation_timeout_value);
    auto& connection = *slot.connection_;
    const auto affected_rows = static_cast<std::uint64_t>(mysql_affected_rows(&connection));
    const auto insert_id = static_cast<std::uint64_t>(mysql_insert_id(&connection));
    auto* raw_result = co_await store_mysql_result(slot, deadline_value);
    if (raw_result != nullptr) {
        free_stored_result(raw_result);
        throw std::invalid_argument("execute() does not accept row-producing SQL");
    }
    if (mysql_field_count(&connection) != 0) {
        throw mysql_error(connection, "mysql_store_result", db_error::code_type::statement_failed);
    }
    co_return db_result_access::make_exec_result(
        affected_rows, insert_id == 0 ? std::nullopt : std::optional<std::uint64_t>(insert_id));
}

task<void> detail::mariadb_pool::execute_control(connection_slot_type& slot, std::string_view sql,
    std::pmr::memory_resource* resource, const ruvia::operation_timeout& operation_timeout_value) {
    (void)co_await execute_on_slot(slot, sql, std::span<const db_value>(), resource, operation_timeout_value);
    co_return;
}

task<db_transaction> detail::mariadb_pool::begin_transaction(
    std::pmr::memory_resource* resource, operation_options operation_options_value,
    db_transaction_options transaction_options) {
    return begin_db_transaction(*this, resource,
        std::move(operation_options_value), make_db_transaction_start_plan(db_driver::mariadb, transaction_options));
}

task<void> detail::mariadb_pool::commit_transaction(
    std::size_t slot, std::pmr::memory_resource* resource, const operation_options& options) {
    return finish_db_transaction(*this, slot, "COMMIT", resource, options);
}

task<void> detail::mariadb_pool::rollback_transaction(
    std::size_t slot, std::pmr::memory_resource* resource, const operation_options& options) {
    return finish_db_transaction(*this, slot, "ROLLBACK", resource, options);
}

void detail::mariadb_pool::abort_transaction(std::size_t slot) noexcept {
    if (slot >= slots_.size()) {
        return;
    }

    close_slot(slots_[slot]);
    release_slot(slot);
}

}  // namespace ruvia
