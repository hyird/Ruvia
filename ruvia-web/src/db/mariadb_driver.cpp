#include <errmsg.h>
#include <mysql.h>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/error.hpp>

#include "ruvia/core/failure_report.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_mysql_runtime.h"
#include "db/db_pipeline_impl.h"
#include "db/db_pool.h"
#include "db/db_sql.h"
#include "db/mariadb_pipeline.h"

namespace ruvia::detail {
namespace {

using mariadb_pool = db_pool<mariadb_driver>;

[[nodiscard]] db_error mysql_socket_error(std::string_view operation, std::error_code error) {
    std::string message(operation);
    if (error) {
        message.append(": ");
        message.append(error_code_text(error));
    }
    return db_error(db_error::code_type::io_error, db_driver::mariadb, std::move(message),
        error ? std::optional<std::int64_t>(error.value()) : std::nullopt);
}

[[nodiscard]] db_slot_socket::native_socket_type mysql_native_socket(MYSQL& connection) noexcept {
    const auto native = mysql_get_socket(&connection);
    using native_socket_type = std::remove_cv_t<decltype(native)>;
    if (native == static_cast<native_socket_type>(MARIADB_INVALID_SOCKET)) {
        return db_slot_socket::invalid_socket;
    }
    return static_cast<db_slot_socket::native_socket_type>(native);
}

// Builds the error for a failed statement call. A statement the server
// rejected ends with its error packet, so the connection is idle again and
// the pool keeps it. A failure that leaves the connection unusable
// (mariadb_connection_failure()), or a result the server still has to send,
// leaves its state unknown: the slot is closed here, before the pool sees a
// statement_failed it would otherwise keep -- unless the caller knows the
// exchange ended (`statement_only`).
[[nodiscard]] db_error mysql_statement_error(
    mariadb_pool& pool, std::size_t index, std::string_view operation, bool statement_only = false) {
    auto& connection = *pool.slot(index).connection_;
    auto error = mysql_error(connection, operation, db_error::code_type::statement_failed);
    if (error.code() == db_error::code_type::statement_failed && !statement_only &&
        (mariadb_connection_failure(mysql_errno(&connection)) || mysql_more_results(&connection) != 0)) {
        pool.close_slot(index);
    }
    return error;
}

// A LOAD ... LOCAL INFILE whose file libmariadb would not send (it answers
// the server's request only for a command that starts with LOAD, so not one
// behind whitespace or a comment): it sent the server an empty file and read
// its response, which a lost connection would have replaced with its own
// error. Only the statement failed.
[[nodiscard]] bool mysql_local_infile_refused(MYSQL& connection, bool exclusive) noexcept {
    return exclusive && mysql_errno(&connection) == CR_UNKNOWN_ERROR;
}

// An unbuffered result borrows its connection, and mysql_free_result() would
// drain the unread rows through it synchronously, or through a connection
// that close_slot() already freed. Detach it first; the caller closes the
// connection, which discards those rows.
void discard_unbuffered_result(void* result_value) noexcept {
    if (auto* raw_result = static_cast<MYSQL_RES*>(result_value); raw_result != nullptr) {
        raw_result->handle = nullptr;
        mysql_free_result(raw_result);
    }
}

// One MariaDB wait: the mask libmariadb asked for, mapped onto a pool slot
// wait, and the outcome mapped back to the mask libmariadb continues with.
//
// A db_config deadline is this pool's to enforce, not the driver's. Reporting
// its expiry to MariaDB as MYSQL_WAIT_TIMEOUT only works where libmariadb has
// a matching timeout option of its own -- it has one for connect, none for a
// statement -- so a query deadline would be handed over and dropped. Failing
// here instead ends the operation, and the pool closes the connection: the
// statement may still be running server-side, which is exactly what a
// client-side timeout means.
struct mysql_wait final {
    mariadb_pool& pool_;
    mysql_wait_deadline_source deadline_source_;
    mariadb_pool::slot_wait wait_;

    [[nodiscard]] bool await_ready() {
        return wait_.await_ready();
    }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept {
        return wait_.await_suspend(handle);
    }
    [[nodiscard]] int await_resume() {
        const auto wait_result = wait_.await_resume();
        if (wait_result.expired_) {
            if (deadline_source_ == mysql_wait_deadline_source::operation) {
                throw pool_.timed_out();
            }
            return MYSQL_WAIT_TIMEOUT;
        }
        if (wait_result.error_) {
            throw mysql_socket_error("waiting for MariaDB socket", wait_result.error_);
        }
        switch (wait_result.ready_) {
            case db_socket_readiness::read:
                return MYSQL_WAIT_READ;
            case db_socket_readiness::write:
                return MYSQL_WAIT_WRITE;
            case db_socket_readiness::error:
                return MYSQL_WAIT_EXCEPT;
            default:
                return MYSQL_WAIT_TIMEOUT;
        }
    }
};

[[nodiscard]] mysql_wait wait_for_mysql(
    mariadb_pool& pool, std::size_t index, int status, const operation_timeout& deadline_value) {
    auto& connection = *pool.slot(index).connection_;
    const auto timeout = deadline_value.remaining();
    if (timeout.has_value() && timeout->count() <= 0) {
        throw pool.timed_out();
    }
    std::optional<std::chrono::milliseconds> driver_timeout;
    if ((status & MYSQL_WAIT_TIMEOUT) != 0) {
        const auto milliseconds = mysql_get_timeout_value_ms(&connection);
        driver_timeout = std::chrono::milliseconds(milliseconds == 0 ? 1 : milliseconds);
    }
    const auto selected_deadline = select_mysql_wait_deadline(timeout, driver_timeout);
    db_socket_readiness wanted = db_socket_readiness::none;
    if ((status & MYSQL_WAIT_READ) != 0) {
        wanted = wanted | db_socket_readiness::read;
    }
    if ((status & MYSQL_WAIT_WRITE) != 0) {
        wanted = wanted | db_socket_readiness::write;
    }
    if ((status & MYSQL_WAIT_EXCEPT) != 0) {
        wanted = wanted | db_socket_readiness::error;
    }

    if (wanted == db_socket_readiness::none) {
        if (!selected_deadline.timeout_) {
            throw db_error(
                db_error::code_type::io_error, db_driver::mariadb, "MariaDB requested an unsupported empty wait");
        }
        return mysql_wait{pool, selected_deadline.source_, pool.sleep(index, selected_deadline.timeout_, deadline_value)};
    }

    const auto native = mysql_native_socket(connection);
    if (native == db_slot_socket::invalid_socket) {
        throw mysql_socket_error("MariaDB returned an invalid socket for an active wait",
            std::make_error_code(std::errc::bad_file_descriptor));
    }
    return mysql_wait{pool, selected_deadline.source_,
        pool.wait_socket(index, native, wanted, selected_deadline.timeout_, deadline_value)};
}

// The session's client character set, read for a string parameter whose
// escape depends on it (mariadb_interpolation_needs_charset()).
task<const MARIADB_CHARSET_INFO*> query_mysql_charset(
    mariadb_pool& pool, std::size_t index, const operation_timeout& deadline_value);

// Nothing else runs on the connection, so the session is current for an
// interpolation that depends on it.
task<void> run_mysql_statement(mariadb_pool& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    auto& connection = *pool.slot(index).connection_;
    auto& statements = pool.slot(index).session_.statements_;
    if (mariadb_names_session_tracking(sql)) {
        statements.untrack();
    }
    std::pmr::string interpolated_sql(pmr_resource_or_default(resource));
    if (!params.empty()) {
        const MARIADB_CHARSET_INFO* charset = nullptr;
        if (mariadb_interpolation_needs_charset(connection, params)) {
            charset = statements.client_utf8mb4() ? &mariadb_utf8mb4_charset()
                                                  : co_await query_mysql_charset(pool, index, deadline_value);
        }
        interpolate_sql(connection, sql, params, interpolated_sql, charset);
        sql = interpolated_sql;
    }
    validate_mariadb_sql_length(sql.size());
    if (mariadb_calls_routine(sql)) {
        statements.routine_called();
    }
    int query_result = 0;
    int status = mysql_real_query_start(&query_result, &connection, sql.data(), static_cast<unsigned long>(sql.size()));
    while (status != 0) {
        status = mysql_real_query_cont(
            &query_result, &connection, co_await wait_for_mysql(pool, index, status, deadline_value));
    }
    if (query_result != 0) {
        throw mysql_statement_error(pool, index, "mysql_real_query",
            mysql_local_infile_refused(connection, mariadb_exclusive_statement(sql)));
    }
}

task<MYSQL_RES*> store_mysql_result(mariadb_pool& pool, std::size_t index, const operation_timeout& deadline_value) {
    auto& connection = *pool.slot(index).connection_;
    MYSQL_RES* result_value = nullptr;
    int status = mysql_store_result_start(&result_value, &connection);
    while (status != 0) {
        status = mysql_store_result_cont(
            &result_value, &connection, co_await wait_for_mysql(pool, index, status, deadline_value));
    }
    co_return result_value;
}

task<const MARIADB_CHARSET_INFO*> query_mysql_charset(
    mariadb_pool& pool, std::size_t index, const operation_timeout& deadline_value) {
    auto& connection = *pool.slot(index).connection_;
    int query_result = 0;
    int status = mysql_real_query_start(&query_result, &connection, mariadb_charset_query.data(),
        static_cast<unsigned long>(mariadb_charset_query.size()));
    while (status != 0) {
        status = mysql_real_query_cont(
            &query_result, &connection, co_await wait_for_mysql(pool, index, status, deadline_value));
    }
    if (query_result != 0) {
        throw mysql_statement_error(pool, index, "reading the MariaDB session character set");
    }
    auto* result = co_await store_mysql_result(pool, index, deadline_value);
    if (result == nullptr) {
        throw mysql_statement_error(pool, index, "reading the MariaDB session character set");
    }
    std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> owned(result, &mysql_free_result);
    co_return &mariadb_queried_charset(*result);
}

// A CALL that returns rows ends with a separate status result, and the
// connection refuses every later command until that result has been read.
// Only status results may follow; a further row set is read and dropped so
// the connection is idle when the statement is refused. Returns whether there
// was one.
task<bool> finish_mysql_results(mariadb_pool& pool, std::size_t index, const operation_timeout& deadline_value) {
    auto& connection = *pool.slot(index).connection_;
    bool extra_row_set = false;
    while (mysql_more_results(&connection) != 0) {
        int next_result = 0;
        int status = mysql_next_result_start(&next_result, &connection);
        while (status != 0) {
            status = mysql_next_result_cont(
                &next_result, &connection, co_await wait_for_mysql(pool, index, status, deadline_value));
        }
        if (next_result != 0) {
            throw mysql_statement_error(pool, index, "mysql_next_result");
        }
        if (mysql_field_count(&connection) != 0) {
            auto* extra = co_await store_mysql_result(pool, index, deadline_value);
            if (extra == nullptr) {
                throw mysql_statement_error(pool, index, "mysql_store_result");
            }
            free_stored_result(extra);
            extra_row_set = true;
        }
    }
    co_return extra_row_set;
}

[[nodiscard]] operation_timeout mysql_stream_deadline(const mariadb_pool& pool, const operation_options& options) {
    return operation_timeout(options.timeout_).constrained_by(pool.config().query_timeout_);
}

// Points the rows of a result `output` already owns at its fields.
void materialize_mysql_rows(db_rows& output, MYSQL_RES& raw_result, std::pmr::memory_resource* resource) {
    const auto field_count = static_cast<std::size_t>(mysql_num_fields(&raw_result));
    const auto row_count = static_cast<std::size_t>(mysql_num_rows(&raw_result));
    auto& result_rows = db_result_access::rows(output);
    auto& result_fields = db_result_access::fields(output);
    auto& result_column_names = db_result_access::column_names(output);
    result_rows.reserve(row_count);
    result_fields.reserve(row_count * field_count);
    result_column_names.reserve(field_count);
    const auto* fields_value = mysql_fetch_fields(&raw_result);
    for (std::size_t i = 0; i < field_count; ++i) {
        result_column_names.emplace_back(fields_value[i].name, static_cast<std::size_t>(fields_value[i].name_length));
    }
    while (auto* row = mysql_fetch_row(&raw_result)) {
        const auto* lengths = mysql_fetch_lengths(&raw_result);
        const auto row_start = result_fields.size();
        for (std::size_t i = 0; i < field_count; ++i) {
            if (row[i] == nullptr) {
                result_fields.push_back(db_result_access::null_field(resource));
                continue;
            }
            result_fields.push_back(db_result_access::borrowed_field(std::string_view(row[i], lengths[i]), resource));
        }
        result_rows.push_back(db_result_access::borrowed_row(
            result_fields.data() + row_start, field_count, result_column_names.data(), result_column_names.size()));
    }
}

// A failed response of a pipelined statement. The server rejected the
// statement alone, so the next response follows; a failure that leaves the
// connection unusable, or results the server still has to send, leave the
// connection out of step and fail it for every statement on it -- unless the
// caller knows the exchange ended (`statement_only`).
[[nodiscard]] std::exception_ptr mysql_pipelined_failure(
    MYSQL& connection, std::string_view operation, bool statement_only = false) {
    auto error = mysql_error(connection, operation, db_error::code_type::statement_failed);
    if (error.code() != db_error::code_type::statement_failed ||
        (!statement_only &&
            (mariadb_connection_failure(mysql_errno(&connection)) || mysql_more_results(&connection) != 0))) {
        throw error;
    }
    return std::make_exception_ptr(std::move(error));
}

using mariadb_group = db_pipeline_group<mariadb_pipeline_io>;

// Records an operation just started: finished at once, or suspended with the
// wait mask exchange() continues it with.
void start_mysql_operation(mariadb_pipeline_io::group_data& data, mariadb_operation operation, int status) noexcept {
    data.operation_ = operation;
    data.status_ = status;
    data.done_ = status == 0;
}

void start_mysql_store(mariadb_pipeline_io::group_data& data, MYSQL& connection) noexcept {
    MYSQL_RES* stored = nullptr;
    const int status = mysql_store_result_start(&stored, &connection);
    data.stored_ = stored;
    start_mysql_operation(data, mariadb_operation::store, status);
}

// After a response of the head statement: its row set, its next result, or
// its end.
void continue_mysql_head(mariadb_pipeline_io::state_type& state, std::size_t index,
    mariadb_pipeline_io::member_type& reader, MYSQL& connection);

// Hands the head statement's outcome on. Only its first result counts for
// affected rows.
void complete_mysql_head(
    mariadb_pipeline_io::state_type& state, std::size_t index, mariadb_pipeline_io::member_type& reader) {
    auto& group = state.group(index);
    auto& data = group.io_;
    auto& head = group.records_[group.head_];
    db_pipeline_outcome outcome;
    if (head.member_ != nullptr) {
        outcome.failure_ = mariadb_statement_failure(data.head_failure_, data.head_extra_row_set_,
            data.head_output_failure_, head.returns_rows_, data.head_returned_rows_);
        if (outcome.failure_ == nullptr && !head.returns_rows_) {
            outcome.affected_rows_ = data.head_affected_rows_;
            if (data.head_insert_id_ != 0) {
                outcome.insert_id_ = data.head_insert_id_;
            }
        }
    }
    data.bytes_in_flight_ -= head.flight_.size_;
    --data.sent_;
    data.head_failure_ = nullptr;
    data.head_output_failure_ = nullptr;
    data.head_affected_rows_ = 0;
    data.head_insert_id_ = 0;
    data.head_returned_rows_ = false;
    data.head_extra_row_set_ = false;
    state.complete_head(index, reader, std::move(outcome));
}

// Hands the outcome of the head statement's prepared run on. A statement the
// cache had to drop as stale runs once more outside a transaction; inside
// one it fails, and the transaction's owner decides what follows.
void complete_mysql_prepared_head(mariadb_pipeline_io::state_type& state, std::size_t index,
    mariadb_pipeline_io::member_type& reader, MYSQL& connection) {
    auto& group = state.group(index);
    auto& data = group.io_;
    auto& head = group.records_[group.head_];
    auto& run = data.prepared_;
    db_pipeline_outcome outcome;
    if (head.member_ != nullptr) {
        if (run.stale_ && head.member_->may_retry_ && (connection.server_status & SERVER_STATUS_IN_TRANS) == 0) {
            outcome.retry_ = true;
        } else {
            outcome.failure_ = mariadb_statement_failure(
                run.failure_, run.extra_row_set_, run.output_failure_, head.returns_rows_, run.returned_rows_);
            if (outcome.failure_ == nullptr && !head.returns_rows_) {
                outcome.affected_rows_ = run.affected_rows_;
                if (run.insert_id_ != 0) {
                    outcome.insert_id_ = run.insert_id_;
                }
            }
        }
    }
    --data.sent_;
    data.prepared_ = {};
    state.complete_head(index, reader, std::move(outcome));
}

void continue_mysql_head(mariadb_pipeline_io::state_type& state, std::size_t index,
    mariadb_pipeline_io::member_type& reader, MYSQL& connection) {
    auto& data = state.group(index).io_;
    if (mysql_field_count(&connection) != 0) {
        start_mysql_store(data, connection);
        return;
    }
    if (mysql_more_results(&connection) != 0) {
        int next_result = 0;
        const int status = mysql_next_result_start(&next_result, &connection);
        data.result_ = next_result;
        start_mysql_operation(data, mariadb_operation::next_result, status);
        return;
    }
    complete_mysql_head(state, index, reader);
}

// Keeps the head statement's first row set for its caller, if it still wants
// it; a failure to keep it (an allocation in the caller's resource) fails only
// that statement, whose responses are still read to their end.
void take_mysql_row_set(mariadb_group& group, MYSQL_RES* stored) {
    auto& data = group.io_;
    auto& head = group.records_[group.head_];
    auto* member = head.member_;
    if (data.head_returned_rows_) {
        data.head_extra_row_set_ = true;
    }
    if (data.head_returned_rows_ || member == nullptr || member->rows_ == nullptr) {
        data.head_returned_rows_ = true;
        free_stored_result(stored);
        return;
    }
    data.head_returned_rows_ = true;
    auto& output = *member->rows_;
    db_result_access::own_raw_result(output, stored, &free_stored_result);
    try {
        materialize_mysql_rows(output, *stored, member->resource_);
    } catch (...) {
        db_result_access::rows(output).clear();
        db_result_access::fields(output).clear();
        db_result_access::column_names(output).clear();
        data.head_output_failure_ = std::current_exception();
    }
}

// Settles the operation exchange() or its start finished.
void finish_mysql_operation(mariadb_pipeline_io::state_type& state, std::size_t index,
    mariadb_pipeline_io::member_type& reader, MYSQL& connection) {
    auto& group = state.group(index);
    auto& data = group.io_;
    const auto operation = std::exchange(data.operation_, mariadb_operation::none);
    data.done_ = false;
    switch (operation) {
        case mariadb_operation::send: {
            if (data.result_ != 0) {
                throw mysql_error(connection, "mysql_send_query", db_error::code_type::io_error);
            }
            auto& record = group.record(data.sent_);
            record.flight_.sent_ = true;
            data.bytes_in_flight_ += record.flight_.size_;
            ++data.sent_;
            if (data.send_buffer_.capacity() > mariadb_pipeline_send_budget &&
                record.flight_.size_ * 4 < data.send_buffer_.capacity()) {
                // Left over from a much larger statement: the connection does
                // not keep its size, while a run of large ones keeps it.
                data.send_buffer_ = std::pmr::string(data.send_buffer_.get_allocator());
            }
            return;
        }
        case mariadb_operation::read:
            if (data.result_ != 0) {
                const auto& head = group.records_[group.head_];
                data.head_failure_ = mysql_pipelined_failure(connection, "mysql_read_query_result",
                    mysql_local_infile_refused(connection, head.flight_.exclusive_));
                complete_mysql_head(state, index, reader);
                return;
            }
            data.head_affected_rows_ = static_cast<std::uint64_t>(mysql_affected_rows(&connection));
            data.head_insert_id_ = static_cast<std::uint64_t>(mysql_insert_id(&connection));
            continue_mysql_head(state, index, reader, connection);
            return;
        case mariadb_operation::charset_query: {
            if (data.result_ != 0) {
                throw mysql_error(connection, "reading the MariaDB session character set", db_error::code_type::io_error);
            }
            MYSQL_RES* stored = nullptr;
            const int status = mysql_store_result_start(&stored, &connection);
            data.stored_ = stored;
            start_mysql_operation(data, mariadb_operation::charset_store, status);
            return;
        }
        case mariadb_operation::charset_store: {
            auto* stored = std::exchange(data.stored_, nullptr);
            if (stored == nullptr) {
                throw mysql_error(connection, "reading the MariaDB session character set", db_error::code_type::io_error);
            }
            std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> owned(stored, &mysql_free_result);
            try {
                group.record(0).flight_.charset_ = &mariadb_queried_charset(*stored);
            } catch (const std::invalid_argument&) {
                // A character set libmariadb cannot escape for: the head fails
                // on its own, unsent.
                db_pipeline_outcome outcome;
                outcome.failure_ = std::current_exception();
                state.complete_head(index, reader, std::move(outcome));
            }
            return;
        }
        case mariadb_operation::store:
            if (auto* stored = std::exchange(data.stored_, nullptr); stored != nullptr) {
                take_mysql_row_set(group, stored);
            } else {
                // A lost connection also yields no result, and libmariadb
                // resets the field count when it drops the connection.
                data.head_failure_ = mysql_pipelined_failure(connection, "mysql_store_result");
                complete_mysql_head(state, index, reader);
                return;
            }
            if (mysql_more_results(&connection) != 0) {
                int next_result = 0;
                const int status = mysql_next_result_start(&next_result, &connection);
                data.result_ = next_result;
                start_mysql_operation(data, mariadb_operation::next_result, status);
                return;
            }
            complete_mysql_head(state, index, reader);
            return;
        case mariadb_operation::next_result:
            if (data.result_ != 0) {
                auto failure = mysql_pipelined_failure(connection, "mysql_next_result");
                if (data.head_failure_ == nullptr) {
                    data.head_failure_ = std::move(failure);
                }
                complete_mysql_head(state, index, reader);
                return;
            }
            continue_mysql_head(state, index, reader, connection);
            return;
        case mariadb_operation::prepared: {
            auto& run = data.prepared_;
            auto& head = group.records_[group.head_];
            auto* member = head.member_;
            if (run.step_ == mariadb_prepared_run::step::execute) {
                if (member == nullptr) {
                    abandon_mariadb_prepared(run);
                } else {
                    start_mysql_operation(data, mariadb_operation::prepared, resume_mariadb_prepared(connection, run, 0));
                    return;
                }
            } else if (run.step_ == mariadb_prepared_run::step::rows) {
                take_mariadb_prepared_rows(
                    run, member == nullptr ? nullptr : member->rows_, member == nullptr ? nullptr : member->resource_);
                start_mysql_operation(data, mariadb_operation::prepared, resume_mariadb_prepared(connection, run, 0));
                return;
            }
            if (run.text_) {
                // The server refused to prepare it, its result is not
                // readable as text, or the session stopped tracking what
                // the cache relies on: it goes out as text.
                head.flight_.sent_ = false;
                --data.sent_;
                data.prepared_ = {};
                return;
            }
            complete_mysql_prepared_head(state, index, reader, connection);
            return;
        }
        case mariadb_operation::none:
            return;
    }
}

}  // namespace

void mariadb_pipeline_io::group_data::reset() noexcept {
    if (stored_ != nullptr) {
        free_stored_result(std::exchange(stored_, nullptr));
    }
    operation_ = mariadb_operation::none;
    status_ = 0;
    done_ = false;
    result_ = 0;
    sent_ = 0;
    bytes_in_flight_ = 0;
    batched_ = false;
    prepared_ = {};
    head_failure_ = nullptr;
    head_output_failure_ = nullptr;
    head_affected_rows_ = 0;
    head_insert_id_ = 0;
    head_returned_rows_ = false;
    head_extra_row_set_ = false;
}

mariadb_pipeline_io::statement_type mariadb_pipeline_io::prepare(
    pool_type& pool, std::string_view sql, std::span<const db_value> params) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    return {sql, params, std::pmr::string(pool.resource())};
}

db_slot_socket::native_socket_type mariadb_pipeline_io::socket(pool_type& pool, std::size_t slot) {
    const auto native = mysql_native_socket(*pool.slot(slot).connection_);
    if (native == db_slot_socket::invalid_socket) {
        throw mysql_socket_error("MariaDB connection has no waitable socket", {});
    }
    return native;
}

// A connection between commands needs nothing to carry a group.
void mariadb_pipeline_io::open(state_type&, std::size_t) {}

// A refused parameter fails the statement, whichever protocol would run it.
// Interpolation happens here unless it depends on the session, which only the
// responses before the statement settle; the command is sent by the reader
// once the connection stands between two responses.
bool mariadb_pipeline_io::queue(
    state_type& state, std::size_t index, member_type&, statement_type& statement, flight_type& flight) {
    if (mariadb_names_session_tracking(statement.sql_)) {
        state.pool().slot(index).session_.statements_.untrack();
    }
    const bool deferred = mariadb_interpolation_needs_session(statement.sql_, statement.params_);
    std::string_view sql = statement.sql_;
    if (!statement.params_.empty() && !deferred) {
        interpolate_sql(
            *state.pool().slot(index).connection_, statement.sql_, statement.params_, statement.interpolated_);
        sql = statement.interpolated_;
    }
    validate_mariadb_sql_length(sql.size());
    flight = {deferred ? std::string_view{} : sql, statement.sql_, statement.params_, 0, false, deferred,
        mariadb_exclusive_statement(statement.sql_), mariadb_calls_routine(statement.sql_)};
    return false;
}

// Between two responses the reader first sends what is queued, within the
// budget, then reads the next response; an operation in progress continues
// once exchange() finished it. After sending it pauses for one worker turn
// before reading, since a read that has to wait for its response blocks every
// send until the response is in: statements arriving in the same turn go out
// with the ones just sent. A cached statement that is the only one its worker
// runs goes prepared at once; statements arriving meanwhile wait for its
// response, as they would behind a text response being read.
bool mariadb_pipeline_io::advance(state_type& state, std::size_t index, member_type& reader) {
    auto& slot = state.pool().slot(index);
    auto& group = slot.session_.pipeline_;
    auto& data = group.io_;
    auto& connection = *slot.connection_;
    if (data.operation_ != mariadb_operation::none) {
        if (!data.done_) {
            return false;
        }
        finish_mysql_operation(state, index, reader, connection);
        return true;
    }
    if (data.sent_ < group.pending_) {
        auto& record = group.record(data.sent_);
        if (record.member_ == nullptr) {
            // Abandoned before it was sent: it is dropped when it is the head.
            ++data.sent_;
            return true;
        }
        // Behind unread responses only what they cannot change goes out:
        // a deferred or exclusive statement waits until it is the head, and
        // nothing follows an exclusive one on the wire.
        const auto& head = group.record(0);
        const bool behind_responses = data.sent_ != 0;
        const bool may_send = !behind_responses || (!record.flight_.deferred_ && !record.flight_.exclusive_ &&
                                                       !(head.flight_.exclusive_ && head.flight_.sent_));
        if (may_send) {
            auto& statements = slot.session_.statements_;
            // A prepared execution cannot share the connection, and it waits
            // for its own response where a text command's wait overlaps
            // others': measured, it only pays when the statement has the
            // worker to itself.
            const bool alone = !behind_responses && group.pending_ == 1 && state.statements() == 1;
            // Each statement counts as one use, however often the reader
            // comes back to it (a character set read first, the send budget
            // full, a prepared run that fell back to text).
            std::optional<mariadb_statement_cache::handle> found;
            if (!record.flight_.exclusive_ && !record.flight_.counted_) {
                record.flight_.counted_ = true;
                found = statements.find(record.flight_.statement_sql_, record.flight_.params_, alone);
            }
            if (found.has_value()) {
                record.flight_.sent_ = true;
                ++data.sent_;
                data.prepared_ = start_mariadb_prepared(statements, *found, record.flight_.params_);
                start_mysql_operation(
                    data, mariadb_operation::prepared, resume_mariadb_prepared(connection, data.prepared_, 0));
                return true;
            }
            if (data.bytes_in_flight_ == 0 ||
                data.bytes_in_flight_ + record.flight_.sql_.size() <= mariadb_pipeline_send_budget) {
                if (record.flight_.deferred_) {
                    if (record.flight_.charset_ == nullptr &&
                        mariadb_interpolation_needs_charset(connection, record.flight_.params_)) {
                        // Its escape depends on the session's client
                        // character set. Every response before it has been
                        // read, so a tracked session's report of it is
                        // current; otherwise a statement before may have
                        // changed it unreported, and it is read first.
                        if (statements.client_utf8mb4()) {
                            record.flight_.charset_ = &mariadb_utf8mb4_charset();
                        } else {
                            int result = 0;
                            const int status = mysql_real_query_start(&result, &connection,
                                mariadb_charset_query.data(), static_cast<unsigned long>(mariadb_charset_query.size()));
                            data.result_ = result;
                            start_mysql_operation(data, mariadb_operation::charset_query, status);
                            return true;
                        }
                    }
                    // Every response before it has been read: the session
                    // libmariadb saw last is the one the server runs it in.
                    try {
                        interpolate_sql(connection, record.flight_.statement_sql_, record.flight_.params_,
                            data.send_buffer_, record.flight_.charset_);
                    } catch (...) {
                        // Its own failure; nothing was sent.
                        db_pipeline_outcome outcome;
                        outcome.failure_ = std::current_exception();
                        state.complete_head(index, reader, std::move(outcome));
                        return true;
                    }
                } else {
                    data.send_buffer_.assign(record.flight_.sql_);
                }
                record.flight_.size_ = data.send_buffer_.size();
                if (record.flight_.routine_) {
                    statements.routine_called();
                }
                int sent = 0;
                const int status = mysql_send_query_start(
                    &sent, &connection, data.send_buffer_.data(), static_cast<unsigned long>(data.send_buffer_.size()));
                data.result_ = sent;
                start_mysql_operation(data, mariadb_operation::send, status);
                data.batched_ = true;
                return true;
            }
        }
    }
    if (!group.records_[group.head_].flight_.sent_) {
        --data.sent_;
        state.complete_head(index, reader, {});
        return true;
    }
    if (data.batched_) {
        data.batched_ = false;
        return false;
    }
    my_bool failed = 0;
    const int status = mysql_read_query_result_start(&failed, &connection);
    data.result_ = failed;
    start_mysql_operation(data, mariadb_operation::read, status);
    return true;
}

// No operation in progress asks for the reader's pause of one turn.
db_socket_readiness mariadb_pipeline_io::wanted(pool_type& pool, std::size_t slot) noexcept {
    const auto& data = pool.slot(slot).session_.pipeline_.io_;
    if (data.operation_ == mariadb_operation::none) {
        return db_socket_readiness::none;
    }
    auto wanted = db_socket_readiness::none;
    if ((data.status_ & MYSQL_WAIT_READ) != 0) {
        wanted = wanted | db_socket_readiness::read;
    }
    if ((data.status_ & MYSQL_WAIT_WRITE) != 0) {
        wanted = wanted | db_socket_readiness::write;
    }
    if ((data.status_ & MYSQL_WAIT_EXCEPT) != 0) {
        wanted = wanted | db_socket_readiness::error;
    }
    // A wait for libmariadb's own timeout alone is bounded by the pool's I/O
    // timeout instead, which needs a socket to wait on.
    return wanted == db_socket_readiness::none ? db_socket_readiness::read : wanted;
}

void mariadb_pipeline_io::check_wait(const db_socket_wait_result& result) {
    if (result.error_ && result.error_ != asio::error::operation_aborted) {
        throw mysql_socket_error("waiting for MariaDB socket", result.error_);
    }
}

// libmariadb's own read and write timeouts are not passed on: the pool bounds
// each stretch without progress itself.
void mariadb_pipeline_io::exchange(state_type& state, std::size_t index, db_socket_readiness ready) {
    auto& slot = state.pool().slot(index);
    auto& data = slot.session_.pipeline_.io_;
    auto& connection = *slot.connection_;
    int mask = 0;
    if (has_db_socket_readiness(ready, db_socket_readiness::read)) {
        mask |= MYSQL_WAIT_READ;
    }
    if (has_db_socket_readiness(ready, db_socket_readiness::write)) {
        mask |= MYSQL_WAIT_WRITE;
    }
    if (has_db_socket_readiness(ready, db_socket_readiness::error)) {
        mask |= MYSQL_WAIT_EXCEPT;
    }
    int status = 0;
    switch (data.operation_) {
        case mariadb_operation::send:
            status = mysql_send_query_cont(&data.result_, &connection, mask);
            break;
        case mariadb_operation::read: {
            my_bool failed = 0;
            status = mysql_read_query_result_cont(&failed, &connection, mask);
            data.result_ = failed;
            break;
        }
        case mariadb_operation::store:
            status = mysql_store_result_cont(&data.stored_, &connection, mask);
            break;
        case mariadb_operation::next_result:
            status = mysql_next_result_cont(&data.result_, &connection, mask);
            break;
        case mariadb_operation::charset_query:
            status = mysql_real_query_cont(&data.result_, &connection, mask);
            break;
        case mariadb_operation::charset_store:
            status = mysql_store_result_cont(&data.stored_, &connection, mask);
            break;
        case mariadb_operation::prepared:
            status = resume_mariadb_prepared(connection, data.prepared_, mask);
            break;
        case mariadb_operation::none:
            return;
    }
    data.status_ = status;
    data.done_ = status == 0;
}

// Responses nobody reads are still on their way while an operation runs or a
// sent statement is unread; a statement such as BEGIN or SET autocommit=0
// leaves the connection inside a transaction.
bool mariadb_pipeline_io::reusable(state_type& state, std::size_t index) noexcept {
    auto& slot = state.pool().slot(index);
    auto& group = slot.session_.pipeline_;
    if (group.io_.operation_ != mariadb_operation::none) {
        return false;
    }
    for (std::size_t offset = 0; offset < group.pending_; ++offset) {
        if (group.record(offset).flight_.sent_) {
            return false;
        }
    }
    unsigned int status = 0;
    if (mariadb_get_infov(slot.connection_, MARIADB_CONNECTION_SERVER_STATUS, &status) != 0) {
        return false;
    }
    return (status & SERVER_STATUS_IN_TRANS) == 0 && (status & SERVER_STATUS_AUTOCOMMIT) != 0;
}

template class db_pipeline_state<mariadb_pipeline_io>;

task<void> mariadb_driver::connect(pool_type& pool, std::size_t index, const operation_timeout& deadline_value) {
    const auto& config = pool.config();
    auto& slot = pool.slot(index);
    slot.session_.statements_.clear();
    auto addresses = co_await pool.resolve_host(index, deadline_value);
    auto resolved_hosts = make_mariadb_resolved_host_list(addresses, pool.resource());

    ensure_mysql_thread_initialized();
    slot.connection_ = mysql_init(nullptr);
    if (slot.connection_ == nullptr) {
        throw db_error(db_error::code_type::connect_failed, db_driver::mariadb, "mysql_init failed");
    }
    auto& initialized = *slot.connection_;
    if (mysql_optionsv(&initialized, MYSQL_OPT_NONBLOCK, nullptr) != 0) {
        throw mysql_error(initialized, "enabling MariaDB non-blocking I/O", db_error::code_type::connect_failed);
    }
    // The statement cache hears every change of the session the server
    // reports, from the handshake on.
    if (!slot.session_.statements_.observe(initialized)) {
        throw mysql_error(initialized, "observing the MariaDB session", db_error::code_type::connect_failed);
    }
    // Pin the connection charset before connecting: utf8mb4 carries full
    // Unicode whatever the server or client library default, and keeps 0x5C
    // meaning only backslash, so escaping a string parameter needs no more
    // than the server status until a statement changes the character set
    // (interpolate_sql()). Set here (not via SET NAMES) so it also governs
    // the handshake.
    if (mysql_optionsv(&initialized, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
        throw mysql_error(initialized, "configuring MariaDB utf8mb4", db_error::code_type::connect_failed);
    }
    if (!set_mysql_timeout(initialized, MYSQL_OPT_CONNECT_TIMEOUT, config.connect_timeout_) ||
        !set_mysql_timeout(initialized, MYSQL_OPT_READ_TIMEOUT, config.read_timeout_) ||
        !set_mysql_timeout(initialized, MYSQL_OPT_WRITE_TIMEOUT, config.write_timeout_)) {
        throw mysql_error(initialized, "configuring MariaDB timeouts", db_error::code_type::connect_failed);
    }
    // The pool keeps one registered duplicate of the connection socket and
    // tells sockets apart by handle value, so the driver must never silently
    // replace its socket. A lost connection fails the operation and the pool
    // reconnects through a fresh connection instead.
    const my_bool reconnect = 0;
    if (mysql_optionsv(&initialized, MYSQL_OPT_RECONNECT, &reconnect) != 0) {
        throw mysql_error(initialized, "disabling MariaDB automatic reconnect", db_error::code_type::connect_failed);
    }

    const bool use_tls = config.tls_.mode_ == client_tls_mode::verify_identity;
    const my_bool enforce_tls = use_tls ? 1 : 0;
    if (mysql_optionsv(&initialized, MYSQL_OPT_SSL_ENFORCE, &enforce_tls) != 0 ||
        mysql_optionsv(&initialized, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &enforce_tls) != 0) {
        throw mysql_error(initialized, "configuring MariaDB authenticated TLS", db_error::code_type::connect_failed);
    }
    if (use_tls) {
        if (mysql_optionsv(&initialized, MYSQL_OPT_TLS_VERSION, "TLSv1.2,TLSv1.3") != 0) {
            throw mysql_error(initialized, "configuring MariaDB TLS versions", db_error::code_type::connect_failed);
        }
        const auto set_path = [&initialized](mysql_option option, const std::pmr::string& path) {
            if (!path.empty() && mysql_optionsv(&initialized, option, path.c_str()) != 0) {
                throw mysql_error(initialized, "configuring MariaDB TLS credentials", db_error::code_type::connect_failed);
            }
        };
        set_path(MYSQL_OPT_SSL_CA, config.tls_.ca_file_);
        set_path(MYSQL_OPT_SSL_CERT, config.tls_.certificate_chain_file_);
        set_path(MYSQL_OPT_SSL_KEY, config.tls_.private_key_file_);
    }
    const auto client_flags = use_tls ? static_cast<unsigned long>(CLIENT_SSL) : 0UL;
    MYSQL* connected = nullptr;
    int status = mysql_real_connect_start(&connected, &initialized, resolved_hosts.c_str(), config.username_.c_str(),
        config.password_.c_str(), config.database_.empty() ? nullptr : config.database_.c_str(), config.port_,
        nullptr, client_flags);
    while (status != 0) {
        status = mysql_real_connect_cont(
            &connected, &initialized, co_await wait_for_mysql(pool, index, status, deadline_value));
    }
    if (connected == nullptr) {
        throw mysql_error(initialized, "mysql_real_connect", db_error::code_type::connect_failed);
    }
    if (use_tls && mysql_get_ssl_cipher(&initialized) == nullptr) {
        throw db_error(db_error::code_type::connect_failed, db_driver::mariadb, "MariaDB refused required TLS");
    }

    // MariaDB's non-blocking API suspends by yielding out of a fibre when
    // the socket reports EAGAIN. Connect leaves that socket in blocking
    // mode, so every later mysql_*_start() ran the entire statement inside
    // the call instead of returning a wait mask: the worker's event loop
    // stopped for as long as the query took -- no other connection on it
    // progressed, no timer fired, and no deadline could be enforced,
    // because nothing was running to enforce it. Switching the descriptor
    // restores the suspend the API is built around.
    const auto native = mysql_native_socket(initialized);
    if (native == db_slot_socket::invalid_socket) {
        throw mysql_socket_error("MariaDB connection has no waitable socket", {});
    }
    if (const auto error = set_db_socket_non_blocking(native); error) {
        throw mysql_socket_error("making MariaDB connection socket non-blocking", error);
    }

    // Prepared statements keep the session state they were parsed in, so the
    // statement cache has to hear of every change of it: tracking of the
    // variables a statement depends on (mariadb_tracked_variables) and of the
    // schema is turned on. A server without session tracking, or one that
    // refuses this, runs nothing prepared.
    bool tracked = false;
    if ((initialized.server_capabilities & CLIENT_SESSION_TRACKING) != 0) {
        int query_result = 0;
        status = mysql_real_query_start(&query_result, &initialized, mariadb_session_tracking_setup.data(),
            static_cast<unsigned long>(mariadb_session_tracking_setup.size()));
        while (status != 0) {
            status = mysql_real_query_cont(
                &query_result, &initialized, co_await wait_for_mysql(pool, index, status, deadline_value));
        }
        if (query_result == 0) {
            tracked = mysql_field_count(&initialized) == 0;
        } else if (mariadb_connection_failure(mysql_errno(&initialized))) {
            throw mysql_error(initialized, "turning on MariaDB session tracking", db_error::code_type::connect_failed);
        }
    }
    slot.session_.statements_.connected(tracked);
}

void mariadb_driver::close_connection(connection_type& connection) noexcept {
    mysql_close(&connection);
}

namespace {

// A statement as text on a connection its caller holds exclusively: its
// result, or what mariadb_statement_failure() makes of its responses.
template <typename result_type>
task<result_type> run_mysql_text(mariadb_pool& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    constexpr bool returns_rows = std::is_same_v<result_type, db_rows>;
    co_await run_mysql_statement(pool, index, sql, params, resource, deadline_value);
    auto& connection = *pool.slot(index).connection_;
    const auto affected_rows = static_cast<std::uint64_t>(mysql_affected_rows(&connection));
    const auto insert_id = static_cast<std::uint64_t>(mysql_insert_id(&connection));
    auto output = db_result_access::make_result(resource);
    auto* raw_result = co_await store_mysql_result(pool, index, deadline_value);
    // A lost connection also yields no result, and libmariadb resets the
    // field count when it drops the connection, so only mysql_errno() tells a
    // failed transfer from a statement without a row set.
    if (raw_result == nullptr && (mysql_field_count(&connection) != 0 || mysql_errno(&connection) != 0)) {
        throw mysql_statement_error(pool, index, "mysql_store_result");
    }
    if (raw_result != nullptr) {
        if constexpr (returns_rows) {
            db_result_access::own_raw_result(output, raw_result, &free_stored_result);
        } else {
            free_stored_result(raw_result);
        }
    }
    const bool extra_row_set = co_await finish_mysql_results(pool, index, deadline_value);
    if (auto failure =
            mariadb_statement_failure(nullptr, extra_row_set, nullptr, returns_rows, raw_result != nullptr)) {
        std::rethrow_exception(failure);
    }
    if constexpr (returns_rows) {
        materialize_mysql_rows(output, *raw_result, resource);
        co_return output;
    } else {
        co_return db_result_access::make_exec_result(
            affected_rows, insert_id == 0 ? std::nullopt : std::optional<std::uint64_t>(insert_id));
    }
}

// A statement on a connection its caller holds exclusively (a transaction's
// statements and control commands): prepared once the connection's cache has
// it, as text otherwise. A statement the server no longer held runs once more
// outside a transaction; inside one it fails, and the transaction's owner
// decides what follows.
template <typename result_type>
task<result_type> run_mysql_cached(mariadb_pool& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    constexpr bool returns_rows = std::is_same_v<result_type, db_rows>;
    auto& slot = pool.slot(index);
    auto& statements = slot.session_.statements_;
    if (mariadb_names_session_tracking(sql)) {
        statements.untrack();
    }
    for (bool may_retry = true;; may_retry = false) {
        const auto found = statements.find(sql, params, true);
        if (!found.has_value()) {
            break;
        }
        auto& connection = *slot.connection_;
        auto output = db_result_access::make_result(resource);
        auto run = start_mariadb_prepared(statements, *found, params);
        while (run.step_ != mariadb_prepared_run::step::done) {
            if (run.step_ == mariadb_prepared_run::step::rows) {
                take_mariadb_prepared_rows(run, returns_rows ? &output : nullptr, resource);
                continue;
            }
            int status = resume_mariadb_prepared(connection, run, 0);
            while (status != 0) {
                status = resume_mariadb_prepared(
                    connection, run, co_await wait_for_mysql(pool, index, status, deadline_value));
            }
        }
        if (run.text_) {
            break;
        }
        if (run.stale_ && may_retry && (connection.server_status & SERVER_STATUS_IN_TRANS) == 0) {
            continue;
        }
        if (auto failure = mariadb_statement_failure(
                run.failure_, run.extra_row_set_, run.output_failure_, returns_rows, run.returned_rows_)) {
            std::rethrow_exception(failure);
        }
        if constexpr (returns_rows) {
            co_return output;
        } else {
            co_return db_result_access::make_exec_result(
                run.affected_rows_, run.insert_id_ == 0 ? std::nullopt : std::optional<std::uint64_t>(run.insert_id_));
        }
    }
    co_return co_await run_mysql_text<result_type>(pool, index, sql, params, resource, deadline_value);
}

}  // namespace

task<db_rows> mariadb_driver::query(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    return run_mysql_cached<db_rows>(pool, index, sql, params, resource, deadline_value);
}

task<db_exec_result> mariadb_driver::execute(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    return run_mysql_cached<db_exec_result>(pool, index, sql, params, resource, deadline_value);
}

task<db_rows> mariadb_driver::query_pipelined(pool_type& pool, std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options, operation_scope* scope) {
    return run_pipelined<mariadb_pipeline_io, db_rows>(
        pool, std::move(sql), std::move(params), resource, std::move(options), scope);
}

task<db_exec_result> mariadb_driver::execute_pipelined(pool_type& pool, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource, operation_options options,
    operation_scope* scope) {
    return run_pipelined<mariadb_pipeline_io, db_exec_result>(
        pool, std::move(sql), std::move(params), resource, std::move(options), scope);
}

task<db_stream_start> mariadb_driver::start_stream(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    co_await run_mysql_statement(pool, index, sql, params, resource, deadline_value);
    auto& connection = *pool.slot(index).connection_;
    auto* raw_result = mysql_use_result(&connection);
    if (raw_result == nullptr) {
        if (mysql_field_count(&connection) != 0 || mysql_errno(&connection) != 0) {
            throw mysql_error(connection, "mysql_use_result", db_error::code_type::statement_failed);
        }
        db_stream_start empty(resource);
        empty.returns_rows_ = false;
        co_return empty;
    }
    db_stream_start start(resource);
    start.result_ = raw_result;
    const auto field_count = static_cast<std::size_t>(mysql_num_fields(raw_result));
    const auto* fields_value = mysql_fetch_fields(raw_result);
    start.column_names_.reserve(field_count);
    for (std::size_t i = 0; i < field_count; ++i) {
        start.column_names_.emplace_back(
            fields_value[i].name, static_cast<std::size_t>(fields_value[i].name_length));
    }
    co_return start;
}

task<std::optional<db_row>> mariadb_driver::read_stream_row(pool_type& pool, std::size_t index, void* result_value,
    std::pmr::vector<db_field>& fields, std::pmr::vector<std::pmr::string>& column_names,
    std::pmr::memory_resource* resource, const operation_options& options) {
    if (result_value == nullptr) {
        co_return std::nullopt;
    }

    auto cancellation = pool.cancel_on_stop(index, options.stop_token_);
    auto* raw_result = static_cast<MYSQL_RES*>(result_value);
    try {
        pool.throw_if_aborted(index);
        const auto deadline_value = mysql_stream_deadline(pool, options);
        MYSQL_ROW row = nullptr;
        int status = mysql_fetch_row_start(&row, raw_result);
        while (status != 0) {
            status = mysql_fetch_row_cont(&row, raw_result, co_await wait_for_mysql(pool, index, status, deadline_value));
        }

        auto& connection = *pool.slot(index).connection_;
        // With mysql_use_result(), a NULL row means either the end of the
        // rows or a failed read; only mysql_errno() tells them apart.
        if (row == nullptr && mysql_errno(&connection) != 0) {
            throw mysql_error(connection, "mysql_fetch_row", db_error::code_type::statement_failed);
        }
        if (row != nullptr) {
            const auto field_count = static_cast<std::size_t>(mysql_num_fields(raw_result));
            const auto* lengths = mysql_fetch_lengths(raw_result);
            fields.clear();
            fields.reserve(field_count);
            for (std::size_t i = 0; i < field_count; ++i) {
                if (row[i] == nullptr) {
                    fields.push_back(db_result_access::null_field(resource));
                    continue;
                }
                fields.push_back(db_result_access::owned_field(std::string_view(row[i], lengths[i]), resource));
            }
            co_return db_result_access::borrowed_row(
                fields.data(), fields.size(), column_names.data(), column_names.size());
        }
    } catch (...) {
        discard_unbuffered_result(raw_result);
        pool.close_slot(index);
        cancellation.finish();
        pool.release_slot(index);
        throw;
    }

    // EOF is not a read failure. The close path owns slot release, including
    // its own failure path, so it must run outside the read-error guard.
    cancellation.finish();
    co_await close_stream(pool, index, result_value, resource, options);
    co_return std::nullopt;
}

task<void> mariadb_driver::close_stream(pool_type& pool, std::size_t index, void* result_value,
    std::pmr::memory_resource*, const operation_options& options) {
    if (result_value == nullptr) {
        co_return;
    }

    auto cancellation = pool.cancel_on_stop(index, options.stop_token_);
    auto* raw_result = static_cast<MYSQL_RES*>(result_value);
    try {
        pool.throw_if_aborted(index);
        const auto deadline_value = mysql_stream_deadline(pool, options);
        int status = mysql_free_result_start(raw_result);
        while (status != 0) {
            status = mysql_free_result_cont(raw_result, co_await wait_for_mysql(pool, index, status, deadline_value));
        }
        raw_result = nullptr;
        // Freeing an unbuffered result drains its unread rows, and a lost
        // connection while draining surfaces only through mysql_errno().
        auto& connection = *pool.slot(index).connection_;
        if (mysql_errno(&connection) != 0) {
            throw mysql_error(connection, "mysql_free_result", db_error::code_type::statement_failed);
        }
        if (co_await finish_mysql_results(pool, index, deadline_value)) {
            std::rethrow_exception(mariadb_statement_failure(nullptr, true, nullptr, true, true));
        }
        cancellation.finish();
        pool.release_slot(index);
    } catch (...) {
        discard_unbuffered_result(raw_result);
        pool.close_slot(index);
        cancellation.finish();
        pool.release_slot(index);
        throw;
    }
}

void mariadb_driver::discard_stream(void* result) noexcept {
    discard_unbuffered_result(result);
}

}  // namespace ruvia::detail
