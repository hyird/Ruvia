#include <errmsg.h>
#include <mysql.h>
#include <mysqld_error.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <coroutine>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/failure_report.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/db/db_session.h"

#include "db/db_pipeline.h"
#include "db/db_pool.h"
#include "db/db_sql_format.h"
#include "http/operation_interrupt.h"

namespace ruvia::detail {
namespace {

using mariadb_pool = db_pool<mariadb_driver>;

class mysql_library_env final {
public:
    mysql_library_env() {
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            throw db_error(db_error::code_type::connect_failed, db_driver::mariadb,
                "failed to initialize the MariaDB client library");
        }
    }

    ~mysql_library_env() {
        mysql_library_end();
    }
};

class mysql_thread_env final {
public:
    mysql_thread_env() {
        if (mysql_thread_init() != 0) {
            throw db_error(db_error::code_type::connect_failed, db_driver::mariadb,
                "failed to initialize the MariaDB client thread");
        }
    }

    ~mysql_thread_env() {
        mysql_thread_end();
    }
};

void ensure_mysql_thread_initialized() {
    static mysql_library_env library_env;
    static thread_local mysql_thread_env thread_env;
    (void)library_env;
    (void)thread_env;
}

// libmariadb's own timeouts are whole seconds; a positive millisecond limit
// rounds up so it never expires early.
[[nodiscard]] bool set_mysql_timeout(
    MYSQL& connection, mysql_option option, std::optional<timeout_duration> timeout) noexcept {
    if (!timeout.has_value()) {
        return true;
    }
    unsigned int seconds = 0;
    if (timeout->value().count() > 0) {
        const auto rounded = std::chrono::ceil<std::chrono::seconds>(timeout->value()).count();
        seconds = std::in_range<unsigned int>(rounded) ? static_cast<unsigned int>(rounded)
                                                       : std::numeric_limits<unsigned int>::max();
    }
    return mysql_optionsv(&connection, option, &seconds) == 0;
}

// Which deadline ends a MariaDB wait: the operation's, or the timeout
// libmariadb asked for itself.
enum class mysql_wait_deadline_source : std::uint8_t {
    none,
    operation,
    driver,
};

struct mysql_wait_deadline final {
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    mysql_wait_deadline_source source_{mysql_wait_deadline_source::none};
};

[[nodiscard]] mysql_wait_deadline select_mysql_wait_deadline(
    std::optional<std::chrono::steady_clock::time_point> operation_deadline_value,
    std::optional<std::chrono::steady_clock::time_point> driver_deadline) noexcept {
    if (operation_deadline_value && (!driver_deadline || *operation_deadline_value <= *driver_deadline)) {
        return {*operation_deadline_value, mysql_wait_deadline_source::operation};
    }
    if (driver_deadline) {
        return {*driver_deadline, mysql_wait_deadline_source::driver};
    }
    return {};
}

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
    db_driver_socket_wait<mariadb_driver> wait_;

    [[nodiscard]] bool await_ready() {
        return wait_.await_ready();
    }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept {
        return wait_.await_suspend(handle);
    }
    [[nodiscard]] int await_resume() {
        const auto wait_result = wait_.resume_raw();
        if (wait_result.expired_) {
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

[[nodiscard]] db_socket_readiness mysql_status_readiness(int status) noexcept {
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
    return wanted;
}

[[nodiscard]] mysql_wait wait_for_mysql(
    mariadb_pool& pool, std::size_t index, int status, const operation_deadline& deadline_value) {
    auto& connection = *pool.slot(index).connection_;
    const auto operation_deadline_value = deadline_value.deadline();
    if (operation_deadline_value && *operation_deadline_value <= std::chrono::steady_clock::now()) {
        throw pool.timed_out();
    }
    std::optional<std::chrono::steady_clock::time_point> driver_deadline;
    if ((status & MYSQL_WAIT_TIMEOUT) != 0) {
        const auto milliseconds = mysql_get_timeout_value_ms(&connection);
        driver_deadline = deadline_after(std::chrono::milliseconds(milliseconds == 0 ? 1 : milliseconds));
    }
    const auto wanted = mysql_status_readiness(status);

    if (wanted == db_socket_readiness::none) {
        const auto selected_deadline = select_mysql_wait_deadline(operation_deadline_value, driver_deadline);
        if (!selected_deadline.deadline_) {
            throw db_error(
                db_error::code_type::io_error, db_driver::mariadb, "MariaDB requested an unsupported empty wait");
        }
        return mysql_wait{db_driver_socket_wait<mariadb_driver>{
            pool, pool.sleep(index, selected_deadline.deadline_, deadline_value),
            selected_deadline.source_ == mysql_wait_deadline_source::operation}};
    }

    const auto native = mysql_native_socket(connection);
    if (native == db_slot_socket::invalid_socket) {
        throw mysql_socket_error("MariaDB returned an invalid socket for an active wait",
            std::make_error_code(std::errc::bad_file_descriptor));
    }
    // Native MariaDB options round to seconds; enforce the public millisecond
    // limit here, like the shared pipeline and PostgreSQL socket paths.
    const auto selected_deadline = select_mysql_wait_deadline(
        deadline_value.constrained_by(db_io_timeout(pool.config(), wanted)).deadline(), driver_deadline);
    return mysql_wait{db_driver_socket_wait<mariadb_driver>{
        pool, pool.wait_socket(index, native, wanted, selected_deadline.deadline_, deadline_value),
        selected_deadline.source_ == mysql_wait_deadline_source::operation}};
}

task<int> exec_mysql_query(
    mariadb_pool& pool, std::size_t index, std::string_view sql, const operation_deadline& deadline_value) {
    auto& connection = *pool.slot(index).connection_;
    int query_result = 0;
    int status = mysql_real_query_start(&query_result, &connection, sql.data(), static_cast<unsigned long>(sql.size()));
    while (status != 0) {
        status = mysql_real_query_cont(
            &query_result, &connection, co_await wait_for_mysql(pool, index, status, deadline_value));
    }
    co_return query_result;
}
// The session's client character set, read for a string parameter whose
// escape depends on it (mariadb_escape_needs_charset()).
task<const MARIADB_CHARSET_INFO*> query_mysql_charset(
    mariadb_pool& pool, std::size_t index, const operation_deadline& deadline_value);

// Nothing else runs on the connection, so the session is current for an
// interpolation that depends on it.
task<void> run_mysql_statement(mariadb_pool& pool, std::size_t index, std::string_view sql,
    const mariadb_statement_classification& classification, std::span<const db_value> params, const operation_deadline& deadline_value) {
    auto& connection = *pool.slot(index).connection_;
    auto& statements = pool.slot(index).session_.statements_;
    if (classification.names_session_tracking_) {
        statements.untrack();
    }
    std::string interpolated_sql;
    if (!params.empty()) {
        const auto param_traits = inspect_mariadb_parameters(params);
        const MARIADB_CHARSET_INFO* charset = nullptr;
        if (mariadb_escape_needs_charset(connection, param_traits.has_ambiguous_backslash_)) {
            charset = statements.client_utf8mb4() ? &mariadb_utf8mb4_charset()
                                                  : co_await query_mysql_charset(pool, index, deadline_value);
        }
        interpolate_sql(connection, sql, params, interpolated_sql, charset);
        sql = interpolated_sql;
    }
    validate_mariadb_sql_length(sql.size());
    if (classification.calls_routine_) {
        statements.routine_called();
    }
    if (co_await exec_mysql_query(pool, index, sql, deadline_value) != 0) {
        throw mysql_statement_error(pool, index, "mysql_real_query",
            mysql_local_infile_refused(connection, classification.exclusive_));
    }
}

task<MYSQL_RES*> store_mysql_result(mariadb_pool& pool, std::size_t index, const operation_deadline& deadline_value) {
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
    mariadb_pool& pool, std::size_t index, const operation_deadline& deadline_value) {
    if (co_await exec_mysql_query(pool, index, mariadb_charset_query, deadline_value) != 0) {
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
task<bool> finish_mysql_results(mariadb_pool& pool, std::size_t index, const operation_deadline& deadline_value) {
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

// Points the rows of a result `output` already owns at its fields.
void materialize_mysql_rows(db_rows& output, MYSQL_RES& raw_result) {
    const auto field_count = static_cast<std::size_t>(mysql_num_fields(&raw_result));
    const auto row_count = static_cast<std::size_t>(mysql_num_rows(&raw_result));
    auto& result_fields = db_result_access::fields(output);
    auto& result_column_names = db_result_access::column_names(output);
    db_result_access::reserve_result(output, row_count, field_count);
    const auto* fields_value = mysql_fetch_fields(&raw_result);
    for (std::size_t i = 0; i < field_count; ++i) {
        result_column_names.emplace_back(fields_value[i].name, static_cast<std::size_t>(fields_value[i].name_length));
    }
    while (auto* row = mysql_fetch_row(&raw_result)) {
        const auto* lengths = mysql_fetch_lengths(&raw_result);
        const auto row_start = result_fields.size();
        for (std::size_t i = 0; i < field_count; ++i) {
            if (row[i] == nullptr) {
                result_fields.push_back(db_result_access::null_field());
                continue;
            }
            result_fields.push_back(db_result_access::borrowed_field(std::string_view(row[i], lengths[i])));
        }
        db_result_access::append_row(output, row_start, field_count);
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
void start_mysql_operation(mariadb_pipeline_io::group_state& state_value, mariadb_operation operation, int status) noexcept {
    state_value.operation_ = operation;
    state_value.status_ = status;
    state_value.done_ = status == 0;
}

void start_mysql_store(mariadb_pipeline_io::group_state& state_value, MYSQL& connection) noexcept {
    MYSQL_RES* stored = nullptr;
    const int status = mysql_store_result_start(&stored, &connection);
    state_value.stored_ = stored;
    start_mysql_operation(state_value, mariadb_operation::store, status);
}

// After a response of the head statement: its row set, its next result, or
// its end.
void continue_mysql_head(mariadb_pipeline_io::state_type& state, std::size_t index,
    mariadb_pipeline_io::member_type& reader, MYSQL& connection);

[[nodiscard]] constexpr db_exec_result make_mysql_exec_result(
    std::uint64_t affected_rows, std::uint64_t insert_id) noexcept {
    return db_result_access::make_exec_result(
        affected_rows, insert_id == 0 ? std::nullopt : std::optional<std::uint64_t>(insert_id));
}

void populate_mysql_outcome(db_pipeline_outcome& outcome, const std::exception_ptr& failure, bool extra_row_set,
    const std::exception_ptr& output_failure, bool returns_rows, bool returned_rows, std::uint64_t affected_rows,
    std::uint64_t insert_id) {
    outcome.failure_ = mariadb_statement_failure(failure, extra_row_set, output_failure, returns_rows, returned_rows);
    if (outcome.failure_ == nullptr && !returns_rows) {
        outcome.affected_rows_ = affected_rows;
        if (insert_id != 0) {
            outcome.insert_id_ = insert_id;
        }
    }
}
// Hands the head statement's outcome on. Only its first result counts for
// affected rows.
void complete_mysql_head(
    mariadb_pipeline_io::state_type& state, std::size_t index, mariadb_pipeline_io::member_type& reader) {
    auto& group = state.group(index);
    auto& data = group.io_;
    auto& head = group.records_[group.head_];
    db_pipeline_outcome outcome;
    if (head.member_ != nullptr) {
        populate_mysql_outcome(outcome, data.head_failure_, data.head_extra_row_set_, data.head_output_failure_,
            head.returns_rows_, data.head_returned_rows_, data.head_affected_rows_, data.head_insert_id_);
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
            populate_mysql_outcome(outcome, run.failure_, run.extra_row_set_, run.output_failure_, head.returns_rows_,
                run.returned_rows_, run.affected_rows_, run.insert_id_);
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
        materialize_mysql_rows(output, *stored);
    } catch (...) {
        db_result_access::clear(output);
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
            const auto size_limit = std::numeric_limits<std::size_t>::max();
            const auto amortized_bytes =
                record.flight_.size_ <= size_limit / 4 ? record.flight_.size_ * 4 : size_limit;
            http_clear_string_retaining_small(
                data.send_buffer_, std::max(mariadb_pipeline_send_budget, amortized_bytes));
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
                    run, member == nullptr ? nullptr : member->rows_);
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

void mariadb_pipeline_io::group_state::reset() noexcept {
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
    std::string_view sql, std::span<const db_value> params) {
    const auto classification = classify_mariadb_statement(sql);
    const auto param_traits = params.empty() ? mariadb_parameter_traits{} : inspect_mariadb_parameters(params);
    const bool deferred = !params.empty() && (classification.has_backslash_ || param_traits.has_backslash_);
    return {sql, params, std::string(), classification, deferred,
        param_traits.has_ambiguous_backslash_};
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
    if (statement.classification_.names_session_tracking_) {
        state.pool().slot(index).session_.statements_.untrack();
    }
    const bool deferred = statement.deferred_;
    std::string_view sql = statement.sql_;
    if (!statement.params_.empty() && !deferred) {
        if (statement.interpolated_.empty()) {
            interpolate_sql(
                *state.pool().slot(index).connection_, statement.sql_, statement.params_, statement.interpolated_);
        }
        sql = statement.interpolated_;
    }
    validate_mariadb_sql_length(sql.size());
    flight = {deferred ? std::string_view{} : sql, statement.sql_, statement.params_,
        (!statement.params_.empty() && !deferred) ? &statement.interpolated_ : nullptr, 0, false, deferred,
        statement.classification_.exclusive_, statement.classification_.calls_routine_, false,
        statement.ambiguous_backslash_};
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
            std::optional<mariadb_statement_cache::entry_ref> found;
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
                        mariadb_escape_needs_charset(connection, record.flight_.ambiguous_backslash_)) {
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
                } else if (record.flight_.interpolated_ != nullptr && !record.flight_.interpolated_->empty()) {
                    data.send_buffer_.swap(*record.flight_.interpolated_);
                    record.flight_.sql_ = data.send_buffer_;
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
    const auto wanted = mysql_status_readiness(data.status_);
    // A wait for libmariadb's own timeout alone is bounded by the pool's I/O
    // timeout instead, which needs a socket to wait on.
    return wanted == db_socket_readiness::none ? db_socket_readiness::read : wanted;
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
// sent statement is unread. release_slot() checks the native session state
// after the group reaches this protocol boundary.
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
    return true;
}

template class db_pipeline_state<mariadb_pipeline_io>;

task<void> mariadb_driver::connect(pool_type& pool, std::size_t index, const operation_deadline& deadline_value) {
    const auto& config = pool.config();
    auto& slot = pool.slot(index);
    slot.session_.statements_.clear();
    auto resolved_hosts =
        make_mariadb_resolved_host_list(co_await pool.resolve_host(index, deadline_value));

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
    const unsigned int local_infile = 0;
    if (mysql_optionsv(&initialized, MYSQL_OPT_LOCAL_INFILE, &local_infile) != 0) {
        throw mysql_error(initialized, "disabling MariaDB LOCAL INFILE", db_error::code_type::connect_failed);
    }

    const bool use_tls = config.tls_.mode_ == client_tls_mode::verify_identity;
    const my_bool enforce_tls = use_tls ? 1 : 0;
    if (mysql_optionsv(&initialized, MYSQL_OPT_SSL_ENFORCE, &enforce_tls) != 0 ||
        mysql_optionsv(&initialized, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &enforce_tls) != 0) {
        throw mysql_error(initialized, "configuring MariaDB authenticated TLS", db_error::code_type::connect_failed);
    }
    if (use_tls) {
        const char* tls_versions = config.tls_.minimum_version_ == tls_version::tls_1_3 ? "TLSv1.3" : config.tls_.minimum_version_ == tls_version::tls_1_2 ? "TLSv1.2,TLSv1.3"
                                                                                                                                                           : "TLSv1.1,TLSv1.2,TLSv1.3";
        if (mysql_optionsv(&initialized, MYSQL_OPT_TLS_VERSION, tls_versions) != 0) {
            throw mysql_error(initialized, "configuring MariaDB TLS versions", db_error::code_type::connect_failed);
        }
        if (mysql_optionsv(&initialized, MYSQL_OPT_SSL_CIPHER, tls_cipher_suites) != 0) {
            throw mysql_error(initialized, "configuring MariaDB TLS ciphers", db_error::code_type::connect_failed);
        }
        const auto set_path = [&initialized](mysql_option option, const tls_file_name& path) {
            if (mysql_optionsv(&initialized, option, tls_file_name_c_str(path)) != 0) {
                throw mysql_error(initialized, "configuring MariaDB TLS credentials", db_error::code_type::connect_failed);
            }
        };
        if (config.tls_.ca_file_.has_value()) {
            set_path(MYSQL_OPT_SSL_CA, *config.tls_.ca_file_);
        }
        if (config.tls_.identity_.has_value()) {
            set_path(MYSQL_OPT_SSL_CERT, config.tls_.identity_->certificate_chain_file_);
            set_path(MYSQL_OPT_SSL_KEY, config.tls_.identity_->private_key_file_);
        }
    }
    const auto client_flags = use_tls ? static_cast<unsigned long>(CLIENT_SSL) : 0UL;
    MYSQL* connected = nullptr;
    int status;
    {
        native_tls_context_scope tls_scope(use_tls ? config.host_ : std::string_view{}, config.tls_.max_outgoing_records_);
        status = mysql_real_connect_start(&connected, &initialized, resolved_hosts.c_str(), config.username_.c_str(),
            config.password_.c_str(), config.database_.empty() ? nullptr : config.database_.c_str(), config.port_,
            nullptr, client_flags);
        tls_scope.rethrow_failure();
    }
    while (status != 0) {
        const auto ready = co_await wait_for_mysql(pool, index, status, deadline_value);
        native_tls_context_scope tls_scope(use_tls ? config.host_ : std::string_view{}, config.tls_.max_outgoing_records_);
        status = mysql_real_connect_cont(
            &connected, &initialized, ready);
        tls_scope.rethrow_failure();
    }
    std::string(resolved_hosts).swap(resolved_hosts);
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
        if (co_await exec_mysql_query(pool, index, mariadb_session_tracking_setup, deadline_value) == 0) {
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

bool mariadb_driver::reusable_connection(connection_type& connection) noexcept {
    unsigned int status = 0;
    if (mysql_more_results(&connection) != 0 ||
        mariadb_get_infov(&connection, MARIADB_CONNECTION_SERVER_STATUS, &status) != 0) {
        return false;
    }
    return (status & SERVER_STATUS_IN_TRANS) == 0 && (status & SERVER_STATUS_AUTOCOMMIT) != 0;
}

// The server sends nothing between commands except the error packet that
// precedes its own close (wait_timeout, KILL, shutdown), so any byte, the end
// of the stream or a socket error retires the connection.
bool mariadb_driver::idle_connection_alive(connection_type& connection) noexcept {
    const auto native = mysql_native_socket(connection);
    return native != db_slot_socket::invalid_socket &&
           peek_db_idle_socket(native) == db_idle_socket_status::quiet;
}

namespace {

// A statement as text on a connection its caller holds exclusively: its
// result, or what mariadb_statement_failure() makes of its responses.
template <typename result_type>
task<result_type> run_mysql_text(mariadb_pool& pool, std::size_t index, std::string_view sql,
    const mariadb_statement_classification& classification, std::span<const db_value> params, const operation_deadline& deadline_value) {
    constexpr bool returns_rows = std::is_same_v<result_type, db_rows>;
    co_await run_mysql_statement(pool, index, sql, classification, params, deadline_value);
    auto& connection = *pool.slot(index).connection_;
    const auto affected_rows = static_cast<std::uint64_t>(mysql_affected_rows(&connection));
    const auto insert_id = static_cast<std::uint64_t>(mysql_insert_id(&connection));
    auto output = db_result_access::make_result();
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
        materialize_mysql_rows(output, *raw_result);
        co_return output;
    } else {
        co_return make_mysql_exec_result(affected_rows, insert_id);
    }
}

// A statement on a connection its caller holds exclusively (a transaction's
// statements and control commands): prepared once the connection's cache has
// it, as text otherwise. A statement the server no longer held runs once more
// outside a transaction; inside one it fails, and the transaction's owner
// decides what follows.
template <typename result_type>
task<result_type> run_mysql_cached(mariadb_pool& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, const operation_deadline& deadline_value) {
    constexpr bool returns_rows = std::is_same_v<result_type, db_rows>;
    auto& slot = pool.slot(index);
    auto& statements = slot.session_.statements_;
    const auto classification = classify_mariadb_statement(sql);
    if (classification.names_session_tracking_) {
        statements.untrack();
    }
    for (bool may_retry = true;; may_retry = false) {
        const auto found = statements.find(sql, params, true);
        if (!found.has_value()) {
            break;
        }
        auto& connection = *slot.connection_;
        auto output = db_result_access::make_result();
        auto run = start_mariadb_prepared(statements, *found, params);
        while (run.step_ != mariadb_prepared_run::step::done) {
            if (run.step_ == mariadb_prepared_run::step::rows) {
                take_mariadb_prepared_rows(run, returns_rows ? &output : nullptr);
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
            co_return make_mysql_exec_result(run.affected_rows_, run.insert_id_);
        }
    }
    co_return co_await run_mysql_text<result_type>(pool, index, sql, classification, params, deadline_value);
}

}  // namespace

task<db_rows> mariadb_driver::query(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, const operation_deadline& deadline_value) {
    return run_mysql_cached<db_rows>(pool, index, sql, params, deadline_value);
}

task<db_exec_result> mariadb_driver::execute(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, const operation_deadline& deadline_value) {
    return run_mysql_cached<db_exec_result>(pool, index, sql, params, deadline_value);
}

task<db_stream_start> mariadb_driver::start_stream(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, const operation_deadline& deadline_value) {
    co_await run_mysql_statement(
        pool, index, sql, classify_mariadb_statement(sql), params, deadline_value);
    auto& connection = *pool.slot(index).connection_;
    std::unique_ptr<MYSQL_RES, decltype(&discard_unbuffered_result)> result(
        mysql_use_result(&connection), &discard_unbuffered_result);
    if (result == nullptr) {
        if (mysql_field_count(&connection) != 0 || mysql_errno(&connection) != 0) {
            throw mysql_error(connection, "mysql_use_result", db_error::code_type::statement_failed);
        }
        db_stream_start empty;
        empty.returns_rows_ = false;
        co_return empty;
    }
    db_stream_start start;
    const auto field_count = static_cast<std::size_t>(mysql_num_fields(result.get()));
    const auto* fields_value = mysql_fetch_fields(result.get());
    start.column_names_.reserve(field_count);
    for (std::size_t i = 0; i < field_count; ++i) {
        start.column_names_.emplace_back(
            fields_value[i].name, static_cast<std::size_t>(fields_value[i].name_length));
    }
    start.result_ = result.release();
    co_return start;
}

namespace {

// Reads an unbuffered result through its end and frees it, then the results
// that may follow it; a further row set fails the stream as for any statement.
// `result_value` is null once freed.
task<void> finish_mysql_stream(
    mariadb_pool& pool, std::size_t index, void*& result_value, const operation_deadline& deadline_value) {
    auto* raw_result = static_cast<MYSQL_RES*>(result_value);
    // The result-set terminator carries the final server status:
    // https://mariadb.com/docs/server/reference/clientserver-protocol/4-server-response-packets/eof_packet
    // libmariadb's free_result drains unread rows without decoding that
    // status, so fetch through EOF before asking whether more results
    // follow. No rows are materialized for an explicitly closed stream.
    while (!raw_result->eof) {
        MYSQL_ROW row = nullptr;
        int status = mysql_fetch_row_start(&row, raw_result);
        while (status != 0) {
            status = mysql_fetch_row_cont(&row, raw_result, co_await wait_for_mysql(pool, index, status, deadline_value));
        }
        if (row == nullptr) {
            break;
        }
    }
    auto& connection = *pool.slot(index).connection_;
    if (mysql_errno(&connection) != 0) {
        throw mysql_error(connection, "mysql_fetch_row", db_error::code_type::statement_failed);
    }
    // EOF detached the native result from its connection; freeing it
    // cannot perform I/O or discard another result's status.
    mysql_free_result(raw_result);
    result_value = nullptr;
    if (co_await finish_mysql_results(pool, index, deadline_value)) {
        std::rethrow_exception(mariadb_statement_failure(nullptr, true, nullptr, true, true));
    }
}

}  // namespace

task<db_stream_step> mariadb_driver::read_stream_row(pool_type& pool, std::size_t index, void*& result_value,
    std::vector<db_field>& fields, std::vector<std::string>& column_names, const operation_deadline& deadline_value) {
    auto* raw_result = static_cast<MYSQL_RES*>(result_value);
    MYSQL_ROW row = nullptr;
    int status = mysql_fetch_row_start(&row, raw_result);
    while (status != 0) {
        status = mysql_fetch_row_cont(&row, raw_result, co_await wait_for_mysql(pool, index, status, deadline_value));
    }
    if (row == nullptr) {
        // With mysql_use_result(), a NULL row means either the end of the
        // rows or a failed read; only mysql_errno() tells them apart.
        auto& connection = *pool.slot(index).connection_;
        if (mysql_errno(&connection) != 0) {
            throw mysql_error(connection, "mysql_fetch_row", db_error::code_type::statement_failed);
        }
        co_await finish_mysql_stream(pool, index, result_value, deadline_value);
        co_return db_stream_end::exhausted;
    }
    const auto field_count = static_cast<std::size_t>(mysql_num_fields(raw_result));
    const auto* lengths = mysql_fetch_lengths(raw_result);
    co_return db_result_access::materialize_stream_row(
        fields, column_names, field_count, [&](std::size_t i) -> std::optional<std::string_view> {
            return row[i] == nullptr ? std::nullopt : std::optional<std::string_view>(std::string_view(row[i], lengths[i]));
        });
}

task<void> mariadb_driver::close_stream(
    pool_type& pool, std::size_t index, void*& result_value, const operation_deadline& deadline_value) {
    return finish_mysql_stream(pool, index, result_value, deadline_value);
}

void mariadb_driver::discard_stream(void* result) noexcept {
    discard_unbuffered_result(result);
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// The text MariaDB's text protocol sends for values its binary protocol
// carries, so a statement returns the same db_field text whichever protocol
// ran it. The server formats a double with my_gcvt() into a buffer of
// FLOATING_POINT_BUFFER - 1 characters, a float the same way with at most
// FLT_DIG significant digits, and a value of fixed scale with my_fcvt(); both
// round with dtoa, which std::to_chars reproduces: the shortest digits that
// read back as the value, or the value correctly rounded when those are too
// many.

// The decimals a field reports when its value has no fixed scale.
constexpr unsigned int mariadb_not_fixed_decimals = 31;
// Room for any formatted value: a double in plain notation reaches about 330
// characters.
constexpr std::size_t mariadb_text_capacity = 400;

using mariadb_text_buffer = std::array<char, mariadb_text_capacity>;

namespace mariadb_text_detail {

// The significant digits of a nonzero finite magnitude and where its decimal
// point falls, as dtoa returns them: 1234 at point 2 is 12.34.
struct digits final {
    std::array<char, 32> text_{};
    int length_{0};
    int point_{0};
};

// Reads std::to_chars' scientific form "d.ddde+XX".
[[nodiscard]] digits parse_scientific(const char* first, const char* last) noexcept {
    digits result;
    const char* cursor = first;
    for (; cursor != last && *cursor != 'e'; ++cursor) {
        if (*cursor != '.') {
            result.text_[static_cast<std::size_t>(result.length_++)] = *cursor;
        }
    }
    int exponent = 0;
    if (cursor != last) {
        (void)std::from_chars(cursor + (cursor[1] == '+' ? 2 : 1), last, exponent);
    }
    while (result.length_ > 1 && result.text_[static_cast<std::size_t>(result.length_ - 1)] == '0') {
        --result.length_;
    }
    result.point_ = exponent + 1;
    return result;
}

// dtoa's mode 4: the shortest digits that read back as `magnitude`, or, when
// those are more than `limit`, `magnitude` correctly rounded to `limit`
// significant digits.
[[nodiscard]] digits significant(double magnitude, int limit) noexcept {
    std::array<char, 64> scratch{};
    auto converted =
        std::to_chars(scratch.data(), scratch.data() + scratch.size(), magnitude, std::chars_format::scientific);
    auto result = parse_scientific(scratch.data(), converted.ptr);
    if (result.length_ > limit) {
        converted = std::to_chars(
            scratch.data(), scratch.data() + scratch.size(), magnitude, std::chars_format::scientific, limit - 1);
        result = parse_scientific(scratch.data(), converted.ptr);
    }
    return result;
}

// Digits in plain notation: "0.00ddd", "dd.ddd" or "ddd00".
[[nodiscard]] char* put_plain(char* out, const digits& found) noexcept {
    if (found.point_ <= 0) {
        *out++ = '0';
        *out++ = '.';
        for (int index = found.point_; index < 0; ++index) {
            *out++ = '0';
        }
    }
    int index = 1;
    for (; index <= found.length_; ++index) {
        *out++ = found.text_[static_cast<std::size_t>(index - 1)];
        if (index == found.point_ && index < found.length_) {
            *out++ = '.';
        }
    }
    for (; index <= found.point_; ++index) {
        *out++ = '0';
    }
    return out;
}

}  // namespace mariadb_text_detail

// my_gcvt() with the server's width: plain notation while it shows every
// significant digit and the point stays within DBL_DIG places of the digits,
// else scientific ("1e20", "1.5e-20").
[[nodiscard]] std::size_t format_mariadb_real(mariadb_text_buffer& buffer, double value, bool single) noexcept {
    namespace text = mariadb_text_detail;
    constexpr int dbl_dig = 15;
    constexpr int flt_dig = 6;
    if (!std::isfinite(value)) {
        // dtoa reports an overflow for infinity and NaN, which my_gcvt()
        // writes as "0".
        buffer[0] = '0';
        return 1;
    }
    int width = 310;
    char* out = buffer.data();
    if (std::signbit(value)) {
        *out++ = '-';
        --width;
    }
    const double magnitude = std::fabs(value);
    if (magnitude == 0) {
        *out++ = '0';
        return static_cast<std::size_t>(out - buffer.data());
    }
    const auto found = text::significant(magnitude, single ? flt_dig : width);
    const int length = found.length_;
    const int point = found.point_;
    // The plain notation's length: "0.00ddd", "dd.ddd" or "ddd00".
    int plain_length = point;
    if (point <= 0) {
        plain_length = length - point + 2;
    } else if (point < length) {
        plain_length = length + 1;
    }
    const bool have_space = plain_length <= width;
    if (have_space && point >= -dbl_dig + 1 && (point <= dbl_dig || length > point)) {
        out = text::put_plain(out, found);
        return static_cast<std::size_t>(out - buffer.data());
    }
    *out++ = found.text_[0];
    if (length > 1) {
        *out++ = '.';
        for (int index = 1; index < length; ++index) {
            *out++ = found.text_[static_cast<std::size_t>(index)];
        }
    }
    *out++ = 'e';
    int exponent = point - 1;
    if (exponent < 0) {
        *out++ = '-';
        exponent = -exponent;
    }
    out = std::to_chars(out, buffer.data() + buffer.size(), exponent).ptr;
    return static_cast<std::size_t>(out - buffer.data());
}

// my_fcvt(): exactly `decimals` digits after the point. dtoa keeps the
// shortest digits that read back as the value when they fit, and pads them
// with zeros; otherwise the value is correctly rounded.
[[nodiscard]] std::size_t format_mariadb_fixed(
    mariadb_text_buffer& buffer, double value, unsigned int decimals) noexcept {
    namespace text = mariadb_text_detail;
    if (!std::isfinite(value)) {
        // As my_gcvt(), my_fcvt() writes dtoa's overflow as "0".
        buffer[0] = '0';
        return 1;
    }
    const double magnitude = std::fabs(value);
    if (magnitude != 0) {
        const auto found = text::significant(magnitude, 17);
        const int fraction = found.length_ > found.point_ ? found.length_ - found.point_ : 0;
        if (fraction <= static_cast<int>(decimals)) {
            char* out = buffer.data();
            if (std::signbit(value)) {
                *out++ = '-';
            }
            out = text::put_plain(out, found);
            if (decimals > 0) {
                if (fraction == 0) {
                    *out++ = '.';
                }
                for (int pad = static_cast<int>(decimals) - fraction; pad > 0; --pad) {
                    *out++ = '0';
                }
            }
            return static_cast<std::size_t>(out - buffer.data());
        }
    }
    const auto converted = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, static_cast<int>(decimals));
    return static_cast<std::size_t>(converted.ptr - buffer.data());
}

// An integer as the text protocol sends it: a ZEROFILL column pads it with
// zeros to the column's display width.
template <typename integer_type>
[[nodiscard]] std::size_t format_mariadb_integer(
    mariadb_text_buffer& buffer, integer_type value, std::size_t zerofill_width) noexcept {
    std::array<char, 24> digits{};
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    const auto length = static_cast<std::size_t>(converted.ptr - digits.data());
    char* out = buffer.data();
    for (std::size_t pad = length; pad < zerofill_width && pad < buffer.size() - digits.size(); ++pad) {
        *out++ = '0';
    }
    for (std::size_t index = 0; index < length; ++index) {
        *out++ = digits[index];
    }
    return static_cast<std::size_t>(out - buffer.data());
}

// The parts of a DATE, DATETIME, TIMESTAMP or TIME value.
struct mariadb_temporal final {
    unsigned int year_{0};
    unsigned int month_{0};
    unsigned int day_{0};
    // A TIME's hours include its days.
    unsigned int hour_{0};
    unsigned int minute_{0};
    unsigned int second_{0};
    unsigned long microsecond_{0};
    bool negative_{false};
};

enum class mariadb_temporal_kind : std::uint8_t { date,
    datetime,
    time };

// "YYYY-MM-DD", "YYYY-MM-DD HH:MM:SS" or "[-]HH:MM:SS", with the column's
// fractional digits. A column without a fixed number of them
// (AUTO_SEC_PART_DIGITS) shows six when the value has a fraction, else none.
[[nodiscard]] std::size_t format_mariadb_temporal(mariadb_text_buffer& buffer, const mariadb_temporal& value,
    mariadb_temporal_kind kind, unsigned int decimals) noexcept {
    if (decimals > 6) {
        decimals = value.microsecond_ != 0 ? 6 : 0;
    }
    char* out = buffer.data();
    const auto put_number = [&out](unsigned long number, int width) {
        for (int index = width - 1; index >= 0; --index) {
            out[index] = static_cast<char>('0' + number % 10);
            number /= 10;
        }
        out += width;
    };
    if (kind != mariadb_temporal_kind::time) {
        put_number(value.year_, 4);
        *out++ = '-';
        put_number(value.month_, 2);
        *out++ = '-';
        put_number(value.day_, 2);
        if (kind == mariadb_temporal_kind::date) {
            return static_cast<std::size_t>(out - buffer.data());
        }
        *out++ = ' ';
        put_number(value.hour_, 2);
    } else {
        if (value.negative_) {
            *out++ = '-';
        }
        put_number(value.hour_, value.hour_ >= 100 ? 3 : 2);
    }
    *out++ = ':';
    put_number(value.minute_, 2);
    *out++ = ':';
    put_number(value.second_, 2);
    if (decimals > 0) {
        *out++ = '.';
        unsigned long fraction = value.microsecond_;
        for (unsigned int drop = 6; drop > decimals; --drop) {
            fraction /= 10;
        }
        put_number(fraction, static_cast<int>(decimals));
    }
    return static_cast<std::size_t>(out - buffer.data());
}

// Column buffers above this many bytes are given back once their rows have
// been read, so a connection does not keep the size of its widest result.
constexpr std::size_t mariadb_column_bytes_kept = 64 * 1024;

static_assert(std::is_same_v<my_bool, char>);
static_assert(alignof(std::max_align_t) >= alignof(MYSQL_BIND));
static_assert(std::is_trivially_copyable_v<MYSQL_BIND>);

[[nodiscard]] MYSQL_BIND* reserve_binds(std::vector<std::max_align_t>& storage, std::size_t count) {
    const std::size_t units = (count * sizeof(MYSQL_BIND) + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    storage.resize(units);
    return reinterpret_cast<MYSQL_BIND*>(storage.data());
}

[[nodiscard]] db_error statement_error(MYSQL_STMT& statement, std::string_view operation,
    db_error::code_type code = db_error::code_type::statement_failed) {
    return mariadb_error(operation, mysql_stmt_errno(&statement), mysql_stmt_sqlstate(&statement),
        mysql_stmt_error(&statement), code);
}

// A failure that leaves the connection out of step, as io_error: the
// connection's own report when it has one, else the statement's.
[[noreturn]] void throw_connection_failure(MYSQL& connection, MYSQL_STMT& statement, std::string_view operation) {
    if (mysql_errno(&connection) != 0) {
        throw mysql_error(connection, operation, db_error::code_type::io_error);
    }
    throw statement_error(statement, operation, db_error::code_type::io_error);
}

[[nodiscard]] bool connection_failure(MYSQL_STMT& statement) noexcept {
    return mariadb_connection_failure(mysql_stmt_errno(&statement));
}

// The type a parameter binds as, when the server reads it as it reads the
// parameter's literal (see mariadb_statement_cache).
[[nodiscard]] std::optional<unsigned int> bound_type(const db_value& param) noexcept {
    switch (param.type()) {
        case db_value_type::null:
            return MYSQL_TYPE_NULL;
        case db_value_type::string:
            if (param.text().find('\0') != std::string_view::npos) {
                return std::nullopt;
            }
            return MYSQL_TYPE_STRING;
        case db_value_type::signed_value:
            if (param.signed_value() < 0) {
                return std::nullopt;
            }
            return MYSQL_TYPE_LONGLONG;
        case db_value_type::unsigned_value:
            if (param.unsigned_value() >
                static_cast<std::uint64_t>((std::numeric_limits<long long>::max)())) {
                return std::nullopt;
            }
            return MYSQL_TYPE_LONGLONG;
        case db_value_type::double_value: {
            const auto value = param.double_value();
            if (!std::isfinite(value) || std::signbit(value)) {
                return std::nullopt;
            }
            return MYSQL_TYPE_DOUBLE;
        }
        case db_value_type::bool_value:
            return MYSQL_TYPE_TINY;
    }
    return std::nullopt;
}

// Whether every column's binary value tells the text the text protocol sends
// for it. A YEAR without ZEROFILL does not: the text protocol pads a table
// column's value to four digits and an expression's not, under the same
// metadata. Metadata from the prepare is not final either: a UNION of YEAR
// columns is described with ZEROFILL when prepared and without it when
// executed. So a statement whose prepared metadata holds any YEAR never runs
// prepared, and an executed one is readable unless a YEAR lacks ZEROFILL.
[[nodiscard]] bool readable_as_text(const MYSQL_FIELD* fields, std::size_t count, bool executed) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (fields[index].type == MYSQL_TYPE_YEAR && (!executed || (fields[index].flags & ZEROFILL_FLAG) == 0)) {
            return false;
        }
    }
    return true;
}

// The name and value of a session variable as libmariadb hands them over.
[[nodiscard]] std::string_view session_text(const MARIADB_CONST_STRING* text) noexcept {
    return text == nullptr || text->str == nullptr ? std::string_view{} : std::string_view(text->str, text->length);
}

// Takes the next item off a comma-separated list, without the spaces around
// it.
[[nodiscard]] std::string_view next_item(std::string_view& list) noexcept {
    const auto comma = list.find(',');
    auto item = list.substr(0, comma);
    list.remove_prefix(comma == std::string_view::npos ? list.size() : comma + 1);
    while (!item.empty() && item.front() == ' ') {
        item.remove_prefix(1);
    }
    while (!item.empty() && item.back() == ' ') {
        item.remove_suffix(1);
    }
    return item;
}

// Whether the comma-separated `list` names `name`, as the server compares
// variable names: regardless of case.
[[nodiscard]] bool lists(std::string_view list, std::string_view name) noexcept {
    while (!list.empty()) {
        if (http_ascii_equals_ignore_case(next_item(list), name)) {
            return true;
        }
    }
    return false;
}

// Whether a value of session_track_system_variables tracks every variable
// the cache relies on.
[[nodiscard]] bool tracks_all(std::string_view value) noexcept {
    if (lists(value, "*")) {
        return true;
    }
    for (auto wanted = mariadb_tracked_variables; !wanted.empty();) {
        if (!lists(value, next_item(wanted))) {
            return false;
        }
    }
    return true;
}

// Reads the tracked list back after a stored procedure may have changed it.
constexpr std::string_view tracking_query = "SELECT @@session_track_system_variables";

// libmariadb's status callback (MARIADB_OPT_STATUS_CALLBACK): each change
// of the session the server reports in an OK packet, while libmariadb
// reads it. It only records; the cache acts on the next find(), which no
// prepared execution is in progress for. Only the schema and the
// variables a statement depends on count: the server also reports others
// it tracks regardless of the list, such as last_gtid after every commit
// when binary logging is on. libmariadb declares the second parameter as
// its C enum; C makes that enum compatible with its underlying integer
// type, which is what this declares, since va_start() after a parameter
// of enum type is undefined in C++.
using status_type = std::underlying_type_t<enum_mariadb_status_info>;
void hear_session_status(void* data, status_type type, ...) {
    if (type != static_cast<status_type>(SESSION_TRACK_TYPE)) {
        return;
    }
    auto& cache = *static_cast<mariadb_statement_cache*>(data);
    std::va_list arguments;
    va_start(arguments, type);
    const auto track = static_cast<enum_session_state_type>(va_arg(arguments, int));
    if (track == SESSION_TRACK_SCHEMA) {
        cache.changed_ = true;
    } else if (track == SESSION_TRACK_SYSTEM_VARIABLES) {
        const auto name = session_text(va_arg(arguments, MARIADB_CONST_STRING*));
        const auto value = session_text(va_arg(arguments, MARIADB_CONST_STRING*));
        if (name == "session_track_system_variables") {
            cache.all_variables_ = tracks_all(value);
        } else if (name == "session_track_schema") {
            cache.schema_tracked_ = value == "ON";
        } else if (name == "character_set_client") {
            cache.client_utf8mb4_ = value == "utf8mb4";
        } else if (name == "character_set_connection") {
            cache.connection_utf8mb4_ = value == "utf8mb4";
        }
        if (name == "collation_connection" || lists(mariadb_tracked_variables, name)) {
            cache.changed_ = true;
        }
    }
    va_end(arguments);
}

}  // namespace

mariadb_statement_cache::mariadb_statement_cache()
    : retired_(),
      types_(),
      binds_(),
      values_(),
      columns_() {}

mariadb_statement_cache::~mariadb_statement_cache() {
    clear();
}

mariadb_statement& mariadb_statement_cache::entry(entry_ref statement) {
    auto* found = statements_.payload(statement);
    if (found == nullptr) {
        throw db_error(
            db_error::code_type::protocol_error, db_driver::mariadb, "MariaDB statement left the cache");
    }
    return *found;
}

void mariadb_statement_cache::reserve_retired(std::size_t count) {
    retired_.reserve(retired_.size() + count);
}

void mariadb_statement_cache::retire(MYSQL_STMT* statement) noexcept {
    if (statement != nullptr) {
        retired_.push_back(statement);
    }
}

void mariadb_statement_cache::refuse(std::size_t hash) noexcept {
    refused_[next_refused_] = hash;
    next_refused_ = (next_refused_ + 1) % refused_.size();
}

void mariadb_statement_cache::refuse(entry_ref statement, std::size_t hash) noexcept {
    (void)statements_.forget(statement, false);
    refuse(hash);
}

bool mariadb_statement_cache::refused(std::size_t hash) const noexcept {
    return std::ranges::find(refused_, hash) != refused_.end();
}

void mariadb_statement_cache::invalidate() {
    std::size_t handles = 0;
    statements_.for_each([&handles](mariadb_statement& statement) {
        handles += statement.handle_ != nullptr ? 1 : 0;
    });
    reserve_retired(handles);
    statements_.for_each([this](mariadb_statement& statement) { retire(std::exchange(statement.handle_, nullptr)); });
    statements_.clear();
    refused_.fill(0);
    changed_ = false;
}

bool mariadb_statement_cache::usable() const noexcept {
    return tracked_ && all_variables_ && schema_tracked_ && !untracked_ && client_utf8mb4_ && connection_utf8mb4_;
}

void mariadb_statement_cache::lose_tracking() noexcept {
    untracked_ = true;
    changed_ = true;
}

// Runs only once the connection is closed (connect() after close_slot(),
// or the slot's end after the pool closed it). A handle whose close was
// suspended has already left the connection's list, so mysql_close() did
// not detach it: detaching each one here keeps mysql_stmt_close() from
// reaching the freed connection.
void mariadb_statement_cache::clear() noexcept {
    const auto release = [](MYSQL_STMT* statement) {
        statement->mysql = nullptr;
        (void)mysql_stmt_close(statement);
    };
    statements_.for_each([&release](mariadb_statement& statement) {
        if (statement.handle_ != nullptr) {
            release(std::exchange(statement.handle_, nullptr));
        }
    });
    for (auto* retired : retired_) {
        release(retired);
    }
    retired_.clear();
    statements_.clear();
    refused_.fill(0);
    // A new connection opens with the utf8mb4 it asks for, and is
    // untracked until it says otherwise.
    tracked_ = false;
    all_variables_ = false;
    schema_tracked_ = false;
    untracked_ = false;
    client_utf8mb4_ = true;
    connection_utf8mb4_ = true;
    changed_ = false;
    verify_ = false;
}

bool mariadb_statement_cache::observe(MYSQL& connection) {
    // libmariadb reads both arguments as void*.
    return mysql_optionsv(&connection, MARIADB_OPT_STATUS_CALLBACK, reinterpret_cast<void*>(&hear_session_status),
               static_cast<void*>(this)) == 0;
}

void mariadb_statement_cache::connected(bool tracked) noexcept {
    tracked_ = tracked;
    all_variables_ = tracked;
    schema_tracked_ = tracked;
    changed_ = true;
}

void mariadb_statement_cache::untrack() noexcept {
    lose_tracking();
}

void mariadb_statement_cache::routine_called() noexcept {
    verify_ = true;
}

bool mariadb_statement_cache::client_utf8mb4() const noexcept {
    return tracked_ && all_variables_ && !untracked_ && !verify_ && client_utf8mb4_;
}

std::optional<mariadb_statement_cache::entry_ref> mariadb_statement_cache::find(
    std::string_view sql, std::span<const db_value> params, bool may_prepare) {
    auto& cache = *this;
    if (cache.changed_) {
        cache.invalidate();
    }
    if (!cache.usable()) {
        return std::nullopt;
    }
    auto& types = cache.types_;
    types.clear();
    types.reserve(params.size());
    for (const auto& param : params) {
        const auto type = bound_type(param);
        if (!type.has_value()) {
            return std::nullopt;
        }
        types.push_back(*type);
    }
    // A statement that may not run prepared is refused inside make() before it
    // could take the place of one that may.
    const auto make = [&cache](std::string_view candidate_sql, std::span<const unsigned int>,
                          std::size_t hash) -> std::optional<mariadb_statement> {
        if (cache.refused(hash)) {
            return std::nullopt;
        }
        const auto traits = mariadb_statement_traits_of(candidate_sql);
        if (!traits.preparable_) {
            cache.refuse(hash);
            return std::nullopt;
        }
        cache.reserve_retired();
        return mariadb_statement{nullptr, traits.integer_params_, hash};
    };
    auto found = cache.statements_.find(sql, types, may_prepare, make);
    if (found.evicted_.has_value()) {
        cache.retire(found.evicted_->handle_);
    }
    if (!may_prepare || !found.statement_.has_value()) {
        return std::nullopt;
    }
    const auto& statement = *cache.statements_.payload(*found.statement_);
    for (std::size_t index = 0; index < types.size() && index < 64; ++index) {
        if (((statement.integer_params_ >> index) & 1U) != 0 &&
            types[index] != static_cast<unsigned int>(MYSQL_TYPE_LONGLONG)) {
            return std::nullopt;
        }
    }
    return found.statement_;
}

std::exception_ptr mariadb_statement_failure(const std::exception_ptr& failure, bool extra_row_set,
    const std::exception_ptr& output_failure, bool returns_rows, bool returned_rows) {
    if (failure != nullptr) {
        return failure;
    }
    if (extra_row_set) {
        return std::make_exception_ptr(std::invalid_argument("SQL must not return more than one result set"));
    }
    if (output_failure != nullptr) {
        return output_failure;
    }
    if (returns_rows && !returned_rows) {
        return std::make_exception_ptr(std::invalid_argument("query() requires row-producing SQL"));
    }
    if (!returns_rows && returned_rows) {
        return std::make_exception_ptr(std::invalid_argument("execute() does not accept row-producing SQL"));
    }
    return nullptr;
}

mariadb_prepared_run start_mariadb_prepared(
    mariadb_statement_cache& cache, mariadb_statement_cache::entry_ref statement, std::span<const db_value> params) noexcept {
    mariadb_prepared_run run;
    run.cache_ = &cache;
    run.handle_ = statement;
    run.params_ = params;
    run.step_ = mariadb_prepared_run::step::close;
    return run;
}

void abandon_mariadb_prepared(mariadb_prepared_run& run) noexcept {
    run.step_ = mariadb_prepared_run::step::done;
}

namespace {

using run_step = mariadb_prepared_run::step;
using cache_state = mariadb_statement_cache;

// Binds the parameters find() accepted with the types it keyed them by.
[[nodiscard]] std::exception_ptr bind_params(
    cache_state& cache, MYSQL_STMT& statement, std::span<const db_value> params) {
    if (mysql_stmt_param_count(&statement) != params.size()) {
        return std::make_exception_ptr(std::invalid_argument("SQL parameter count does not match placeholders"));
    }
    auto* binds = reserve_binds(cache.binds_, params.size());
    cache.values_.resize(params.size());
    for (std::size_t index = 0; index < params.size(); ++index) {
        auto& bind = binds[index];
        std::memset(&bind, 0, sizeof(bind));
        auto& value = cache.values_[index];
        const auto& param = params[index];
        switch (param.type()) {
            case db_value_type::null:
                bind.buffer_type = MYSQL_TYPE_NULL;
                break;
            case db_value_type::string: {
                const auto text = param.text();
                bind.buffer_type = MYSQL_TYPE_STRING;
                bind.buffer = const_cast<char*>(text.data());
                bind.buffer_length = static_cast<unsigned long>(text.size());
                break;
            }
            case db_value_type::signed_value:
                value.integer_ = param.signed_value();
                bind.buffer_type = MYSQL_TYPE_LONGLONG;
                bind.buffer = &value.integer_;
                break;
            case db_value_type::unsigned_value:
                // At most INT64_MAX: signed, as its literal reads.
                value.integer_ = static_cast<long long>(param.unsigned_value());
                bind.buffer_type = MYSQL_TYPE_LONGLONG;
                bind.buffer = &value.integer_;
                break;
            case db_value_type::double_value:
                value.real_ = param.double_value();
                bind.buffer_type = MYSQL_TYPE_DOUBLE;
                bind.buffer = &value.real_;
                break;
            case db_value_type::bool_value:
                value.flag_ = param.bool_value() ? 1 : 0;
                bind.buffer_type = MYSQL_TYPE_TINY;
                bind.buffer = &value.flag_;
                break;
        }
    }
    if (!params.empty() && mysql_stmt_bind_param(&statement, binds) != 0) {
        return std::make_exception_ptr(statement_error(statement, "mysql_stmt_bind_param"));
    }
    return nullptr;
}

// Binds each column to the C type its binary value arrives in; text-like
// values go to a buffer as long as the longest one stored.
MYSQL_BIND* bind_results(cache_state& cache, const MYSQL_FIELD* fields, std::size_t count) {
    auto* binds = reserve_binds(cache.binds_, count);
    while (cache.columns_.size() < count) {
        cache.columns_.emplace_back(mariadb_column{std::vector<char>()});
    }
    cache.column_needed_ = 0;
    for (std::size_t index = 0; index < count; ++index) {
        auto& bind = binds[index];
        auto& column = cache.columns_[index];
        const auto& field = fields[index];
        std::memset(&bind, 0, sizeof(bind));
        std::size_t size = 0;
        switch (field.type) {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_YEAR:
                bind.buffer_type = MYSQL_TYPE_LONGLONG;
                bind.is_unsigned = (field.flags & UNSIGNED_FLAG) != 0 ? 1 : 0;
                size = sizeof(long long);
                break;
            case MYSQL_TYPE_FLOAT:
                bind.buffer_type = MYSQL_TYPE_FLOAT;
                size = sizeof(float);
                break;
            case MYSQL_TYPE_DOUBLE:
                bind.buffer_type = MYSQL_TYPE_DOUBLE;
                size = sizeof(double);
                break;
            case MYSQL_TYPE_DATE:
            case MYSQL_TYPE_DATETIME:
            case MYSQL_TYPE_TIMESTAMP:
            case MYSQL_TYPE_TIME:
                bind.buffer_type = field.type;
                size = sizeof(MYSQL_TIME);
                break;
            default:
                bind.buffer_type = MYSQL_TYPE_STRING;
                size = static_cast<std::size_t>(field.max_length) + 1;
                break;
        }
        // Aligned for every bound C type.
        column.buffer_.resize(std::max(size, sizeof(MYSQL_TIME)) + alignof(MYSQL_TIME));
        cache.column_needed_ += column.buffer_.size();
        auto* storage = column.buffer_.data();
        auto space = column.buffer_.size();
        void* aligned = storage;
        (void)std::align(alignof(MYSQL_TIME), size, aligned, space);
        bind.buffer = aligned;
        bind.buffer_length = static_cast<unsigned long>(size);
        bind.length = &column.length_;
        bind.is_null = &column.null_;
        bind.error = &column.error_;
    }
    cache.column_bytes_ = 0;
    for (const auto& column : cache.columns_) {
        cache.column_bytes_ += column.buffer_.capacity();
    }
    return binds;
}

// The text the text protocol sends for the column's fetched value.
[[nodiscard]] std::string_view column_text(
    const MYSQL_FIELD& field, const MYSQL_BIND& bind, const mariadb_column& column, mariadb_text_buffer& buffer) {
    const auto zerofill = (field.flags & ZEROFILL_FLAG) != 0 ? static_cast<std::size_t>(field.length) : 0;
    std::size_t length = 0;
    switch (bind.buffer_type) {
        case MYSQL_TYPE_LONGLONG: {
            long long value = 0;
            std::memcpy(&value, bind.buffer, sizeof(value));
            length = bind.is_unsigned != 0
                         ? format_mariadb_integer(buffer, static_cast<unsigned long long>(value), zerofill)
                         : format_mariadb_integer(buffer, value, zerofill);
            break;
        }
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE: {
            double value = 0;
            if (bind.buffer_type == MYSQL_TYPE_FLOAT) {
                float single = 0;
                std::memcpy(&single, bind.buffer, sizeof(single));
                value = single;
            } else {
                std::memcpy(&value, bind.buffer, sizeof(value));
            }
            length = field.decimals >= mariadb_not_fixed_decimals
                         ? format_mariadb_real(buffer, value, bind.buffer_type == MYSQL_TYPE_FLOAT)
                         : format_mariadb_fixed(buffer, value, field.decimals);
            if (zerofill > length) {
                std::memmove(buffer.data() + (zerofill - length), buffer.data(), length);
                std::memset(buffer.data(), '0', zerofill - length);
                length = zerofill;
            }
            break;
        }
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIME: {
            MYSQL_TIME value{};
            std::memcpy(&value, bind.buffer, sizeof(value));
            const mariadb_temporal parts{value.year, value.month, value.day, value.hour, value.minute, value.second,
                value.second_part, value.neg != 0};
            const auto kind = bind.buffer_type == MYSQL_TYPE_DATE   ? mariadb_temporal_kind::date
                              : bind.buffer_type == MYSQL_TYPE_TIME ? mariadb_temporal_kind::time
                                                                    : mariadb_temporal_kind::datetime;
            length = format_mariadb_temporal(buffer, parts, kind, field.decimals);
            break;
        }
        default:
            return std::string_view(static_cast<const char*>(bind.buffer), column.length_);
    }
    return std::string_view(buffer.data(), length);
}

// Fetches every stored row and keeps it, as text, in `output`. The fields
// are those of the statement's current result, which libmariadb updates on
// each execution.
void materialize_rows(cache_state& cache, MYSQL_STMT& statement, db_rows& output) {
    const auto field_count = static_cast<std::size_t>(statement.field_count);
    const auto* fields = statement.fields;
    auto* binds = bind_results(cache, fields, field_count);
    if (mysql_stmt_bind_result(&statement, binds) != 0) {
        throw statement_error(statement, "mysql_stmt_bind_result");
    }
    const auto row_count = static_cast<std::size_t>(mysql_stmt_num_rows(&statement));
    auto& values = db_result_access::fields(output);
    auto& names = db_result_access::column_names(output);
    db_result_access::reserve_result(output, row_count, field_count);
    for (std::size_t index = 0; index < field_count; ++index) {
        names.emplace_back(fields[index].name, static_cast<std::size_t>(fields[index].name_length));
    }
    mariadb_text_buffer buffer;
    for (;;) {
        const int fetched = mysql_stmt_fetch(&statement);
        if (fetched == MYSQL_NO_DATA) {
            break;
        }
        if (fetched != 0) {
            throw statement_error(statement, "mysql_stmt_fetch");
        }
        const auto row_start = values.size();
        for (std::size_t index = 0; index < field_count; ++index) {
            const auto& column = cache.columns_[index];
            if (column.null_ != 0) {
                values.push_back(db_result_access::null_field());
                continue;
            }
            values.push_back(
                db_result_access::owned_field(column_text(fields[index], binds[index], column, buffer)));
        }
        db_result_access::append_row(output, row_start, field_count);
    }
}

// Drops a statement whose result the run cannot use, and leaves that
// result's rows to the draining step: forgotten, so that its next use
// prepares it anew, or refused, so that it runs as text. The statement has
// run, so it is not run again.
void start_draining(cache_state& cache, mariadb_prepared_run& run, mariadb_statement& entry, bool refuse) {
    cache.reserve_retired();
    auto* statement = std::exchange(entry.handle_, nullptr);
    if (refuse) {
        cache.refuse(run.handle_, entry.key_hash_);
    } else {
        (void)cache.statements_.forget(run.handle_, true);
    }
    cache.retire(statement);
    run.closing_ = statement;
    run.step_ = run_step::draining;
}

}  // namespace

void take_mariadb_prepared_rows(mariadb_prepared_run& run, db_rows* output) {
    auto& cache = *run.cache_;
    auto& entry = cache.entry(run.handle_);
    auto& statement = *entry.handle_;
    if (run.returned_rows_) {
        run.extra_row_set_ = true;
    } else if (output != nullptr) {
        try {
            materialize_rows(cache, statement, *output);
        } catch (...) {
            db_result_access::clear(*output);
            run.output_failure_ = std::current_exception();
        }
    }
    if (cache.column_bytes_ > mariadb_column_bytes_kept) {
        cache.columns_.clear();
        cache.column_bytes_ = 0;
    }
    run.returned_rows_ = true;
    (void)mysql_stmt_free_result(&statement);
    run.step_ = mysql_stmt_more_results(&statement) != 0 ? run_step::next_result : run_step::done;
}

int resume_mariadb_prepared(MYSQL& connection, mariadb_prepared_run& run, int ready) {
    auto& cache = *run.cache_;
    for (;;) {
        switch (run.step_) {
            case run_step::verifying: {
                int failed = 0;
                const int status = run.started_ ? mysql_real_query_cont(&failed, &connection, ready)
                                                : mysql_real_query_start(&failed, &connection, tracking_query.data(),
                                                      static_cast<unsigned long>(tracking_query.size()));
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                if (failed != 0) {
                    if (mariadb_connection_failure(mysql_errno(&connection))) {
                        throw mysql_error(connection, "reading MariaDB session tracking", db_error::code_type::io_error);
                    }
                    cache.lose_tracking();
                    run.text_ = true;
                    run.step_ = run_step::done;
                    return 0;
                }
                run.step_ = run_step::verified;
                continue;
            }
            case run_step::verified: {
                MYSQL_RES* result = nullptr;
                const int status = run.started_ ? mysql_store_result_cont(&result, &connection, ready)
                                                : mysql_store_result_start(&result, &connection);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                if (result == nullptr && mariadb_connection_failure(mysql_errno(&connection))) {
                    throw mysql_error(connection, "reading MariaDB session tracking", db_error::code_type::io_error);
                }
                const std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> owned(result, &mysql_free_result);
                auto* row = result == nullptr || mysql_num_fields(result) != 1 ? nullptr : mysql_fetch_row(result);
                const auto* lengths = row == nullptr ? nullptr : mysql_fetch_lengths(result);
                if (lengths == nullptr || row[0] == nullptr || !tracks_all(std::string_view(row[0], lengths[0]))) {
                    // A procedure changed the list without a report: the
                    // session's later changes may go unheard.
                    cache.lose_tracking();
                    run.text_ = true;
                    run.step_ = run_step::done;
                    return 0;
                }
                cache.verify_ = false;
                run.step_ = run_step::close;
                continue;
            }
            case run_step::close: {
                if (!run.started_) {
                    if (cache.verify_) {
                        run.step_ = run_step::verifying;
                        continue;
                    }
                    if (cache.retired_.empty()) {
                        run.step_ = cache.entry(run.handle_).handle_ == nullptr ? run_step::prepare : run_step::execute;
                        if (run.step_ == run_step::execute) {
                            return 0;
                        }
                        continue;
                    }
                    run.closing_ = cache.retired_.back();
                }
                my_bool failed = 0;
                const int status = run.started_ ? mysql_stmt_close_cont(&failed, run.closing_, ready)
                                                : mysql_stmt_close_start(&failed, run.closing_);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                // Freed whether or not the server heard of it.
                cache.retired_.pop_back();
                run.closing_ = nullptr;
                if (failed != 0) {
                    throw mysql_error(connection, "mysql_stmt_close", db_error::code_type::io_error);
                }
                continue;
            }
            case run_step::prepare: {
                auto& entry = cache.entry(run.handle_);
                if (!run.started_) {
                    cache.reserve_retired();
                    auto* created = mysql_stmt_init(&connection);
                    if (created == nullptr) {
                        throw std::bad_alloc();
                    }
                    entry.handle_ = created;
                    const my_bool update_max_length = 1;
                    (void)mysql_stmt_attr_set(created, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_length);
                }
                // The cache owns the text libmariadb sends, also while the
                // prepare is suspended and the statement's caller has left.
                const auto sql = cache.statements_.sql(run.handle_);
                int failed = 0;
                const int status = run.started_ ? mysql_stmt_prepare_cont(&failed, entry.handle_, ready)
                                                : mysql_stmt_prepare_start(&failed, entry.handle_, sql.data(),
                                                      static_cast<unsigned long>(sql.size()));
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                auto& statement = *entry.handle_;
                if (failed != 0) {
                    if (connection_failure(statement)) {
                        throw_connection_failure(connection, statement, "mysql_stmt_prepare");
                    }
                    // Never prepared, so closing it sends nothing.
                    (void)mysql_stmt_close(std::exchange(entry.handle_, nullptr));
                    cache.refuse(run.handle_, entry.key_hash_);
                    run.text_ = true;
                    run.step_ = run_step::done;
                    return 0;
                }
                if (!readable_as_text(statement.fields, statement.field_count, false)) {
                    // Prepared on the server: closed by the next run.
                    cache.retire(std::exchange(entry.handle_, nullptr));
                    cache.refuse(run.handle_, entry.key_hash_);
                    run.text_ = true;
                    run.step_ = run_step::done;
                    return 0;
                }
                run.step_ = run_step::execute;
                return 0;
            }
            case run_step::execute:
            case run_step::executing: {
                auto& entry = cache.entry(run.handle_);
                auto& statement = *entry.handle_;
                if (run.step_ == run_step::execute) {
                    if (auto failure = bind_params(cache, statement, run.params_); failure != nullptr) {
                        run.failure_ = std::move(failure);
                        run.step_ = run_step::done;
                        return 0;
                    }
                    run.step_ = run_step::executing;
                }
                int failed = 0;
                const int status = run.started_ ? mysql_stmt_execute_cont(&failed, &statement, ready)
                                                : mysql_stmt_execute_start(&failed, &statement);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                if (failed != 0) {
                    const auto code = mysql_stmt_errno(&statement);
                    if (code == CR_NEW_STMT_METADATA) {
                        // The server prepared it anew, a table having
                        // changed, and ran it: its rows, of a shape the
                        // handle was not prepared for, are on their way.
                        run.failure_ = std::make_exception_ptr(mariadb_error("mysql_stmt_execute", code,
                            mysql_stmt_sqlstate(&statement),
                            "the statement's result changed shape since it was prepared; it is prepared anew on its "
                            "next use",
                            db_error::code_type::statement_failed));
                        start_draining(cache, run, entry, false);
                        continue;
                    }
                    if (connection_failure(statement)) {
                        throw_connection_failure(connection, statement, "mysql_stmt_execute");
                    }
                    run.failure_ = std::make_exception_ptr(statement_error(statement, "mysql_stmt_execute"));
                    if (code == ER_NEED_REPREPARE || code == ER_UNKNOWN_STMT_HANDLER) {
                        // The server no longer runs it as prepared.
                        cache.reserve_retired();
                        cache.retire(std::exchange(entry.handle_, nullptr));
                        (void)cache.statements_.forget(run.handle_, true);
                        run.stale_ = true;
                    }
                    run.step_ = run_step::done;
                    return 0;
                }
                run.affected_rows_ = mysql_stmt_affected_rows(&statement);
                run.insert_id_ = mysql_stmt_insert_id(&statement);
                if (mysql_stmt_field_count(&statement) != 0) {
                    if (!readable_as_text(statement.fields, statement.field_count, true)) {
                        run.failure_ = std::make_exception_ptr(db_error(db_error::code_type::statement_failed,
                            db_driver::mariadb,
                            "MariaDB prepared statement result has a YEAR column whose text the binary protocol does "
                            "not tell; the statement runs as text from its next use"));
                        start_draining(cache, run, entry, true);
                        continue;
                    }
                    run.step_ = run_step::storing;
                    continue;
                }
                run.step_ = mysql_stmt_more_results(&statement) != 0 ? run_step::next_result : run_step::done;
                if (run.step_ == run_step::done) {
                    return 0;
                }
                continue;
            }
            case run_step::storing: {
                auto& statement = *cache.entry(run.handle_).handle_;
                int failed = 0;
                const int status = run.started_ ? mysql_stmt_store_result_cont(&failed, &statement, ready)
                                                : mysql_stmt_store_result_start(&failed, &statement);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                if (failed != 0) {
                    if (connection_failure(statement)) {
                        throw_connection_failure(connection, statement, "mysql_stmt_store_result");
                    }
                    if (run.failure_ == nullptr) {
                        run.failure_ = std::make_exception_ptr(statement_error(statement, "mysql_stmt_store_result"));
                    }
                    // The server's error ended the rows, but libmariadb still
                    // holds the statement as waiting for its result: it would
                    // refuse to close or reset any other statement of the
                    // connection, and freeing the result would leave it
                    // expecting more rows on its next execution.
                    run.step_ = run_step::resetting;
                    continue;
                }
                run.step_ = run_step::rows;
                return 0;
            }
            case run_step::next_result: {
                auto& statement = *cache.entry(run.handle_).handle_;
                int result = 0;
                const int status = run.started_ ? mysql_stmt_next_result_cont(&result, &statement, ready)
                                                : mysql_stmt_next_result_start(&result, &statement);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                if (result > 0) {
                    if (connection_failure(statement)) {
                        throw_connection_failure(connection, statement, "mysql_stmt_next_result");
                    }
                    if (run.failure_ == nullptr) {
                        run.failure_ = std::make_exception_ptr(statement_error(statement, "mysql_stmt_next_result"));
                    }
                    run.step_ = run_step::done;
                    return 0;
                }
                if (result < 0) {
                    run.step_ = run_step::done;
                    return 0;
                }
                if (mysql_stmt_field_count(&statement) != 0) {
                    run.step_ = run_step::storing;
                    continue;
                }
                if (mysql_stmt_more_results(&statement) == 0) {
                    run.step_ = run_step::done;
                    return 0;
                }
                continue;
            }
            case run_step::resetting: {
                auto& entry = cache.entry(run.handle_);
                auto& statement = *entry.handle_;
                my_bool failed = 0;
                const int status = run.started_ ? mysql_stmt_reset_cont(&failed, &statement, ready)
                                                : mysql_stmt_reset_start(&failed, &statement);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                if (failed != 0) {
                    if (connection_failure(statement)) {
                        throw_connection_failure(connection, statement, "mysql_stmt_reset");
                    }
                    // The server no longer knows it: it is prepared anew.
                    cache.reserve_retired();
                    cache.retire(std::exchange(entry.handle_, nullptr));
                    (void)cache.statements_.forget(run.handle_, true);
                }
                run.step_ = run_step::done;
                return 0;
            }
            case run_step::draining: {
                auto& statement = *run.closing_;
                my_bool failed = 0;
                const int status = run.started_ ? mysql_stmt_free_result_cont(&failed, &statement, ready)
                                                : mysql_stmt_free_result_start(&failed, &statement);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                run.closing_ = nullptr;
                if (failed != 0) {
                    throw_connection_failure(connection, statement, "mysql_stmt_free_result");
                }
                run.step_ = run_step::done;
                return 0;
            }
            case run_step::rows:
            case run_step::done:
                return 0;
        }
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

[[nodiscard]] std::size_t mariadb_string_literal_size_hint(std::size_t value_size) {
    // mysql_real_escape_string may expand every input byte to two output
    // bytes, and its input/output lengths are both unsigned long. Check both
    // bounds before doing the multiplication or passing the input to C.
    if (value_size > (std::numeric_limits<unsigned long>::max)() / 2 ||
        value_size > ((std::numeric_limits<std::size_t>::max)() - 2) / 2) {
        throw std::length_error("MariaDB string parameter is too large");
    }
    return value_size * 2 + 2;
}

// A backslash in `value` that a backslash put in front of it could join to
// the byte before: one that follows a byte above 0x7F, which a multibyte
// character set may read as a character's lead byte.
[[nodiscard]] bool ambiguous_backslash(std::string_view value) noexcept {
    for (auto backslash = value.find('\\'); backslash != std::string_view::npos;
        backslash = value.find('\\', backslash + 1)) {
        if (backslash != 0 && static_cast<unsigned char>(value[backslash - 1]) > 0x7f) {
            return true;
        }
    }
    return false;
}

// A string literal: each quote doubled, which reads the same in every
// sql_mode and character set, as no character set has a multibyte character
// ending in a quote byte; and, when the session reads backslash escapes, each
// backslash doubled. `charset` keeps a multibyte character whole, so that a
// backslash byte inside one is not escaped; without it, no backslash may
// follow a byte above 0x7F (ambiguous_backslash()), and an ASCII byte never
// joins the backslash put after it.
void append_string_literal(std::string& output, std::string_view value, bool backslash_escapes,
    const MARIADB_CHARSET_INFO* charset) {
    const bool multibyte = backslash_escapes && charset != nullptr && charset->char_maxlen > 1 &&
                           value.find('\\') != std::string_view::npos;
    output.push_back('\'');
    if (!multibyte) {
        const auto special = backslash_escapes ? std::string_view("'\\", 2) : std::string_view("'", 1);
        for (auto at = value.find_first_of(special); at != std::string_view::npos; at = value.find_first_of(special)) {
            output.append(value.substr(0, at + 1));
            output.push_back(value[at]);
            value.remove_prefix(at + 1);
        }
        output.append(value);
    } else {
        const char* const end = value.data() + value.size();
        for (const char* cursor = value.data(); cursor != end;) {
            if (const auto length = charset->mb_valid(cursor, end); length > 1) {
                output.append(cursor, length);
                cursor += length;
                continue;
            }
            const char byte = *cursor++;
            if (byte == '\'' || byte == '\\') {
                output.push_back(byte);
            }
            output.push_back(byte);
        }
    }
    output.push_back('\'');
}

[[nodiscard]] std::size_t value_literal_size_hint(const db_value& value) {
    switch (value.type()) {
        case db_value_type::null:
            return 4;
        case db_value_type::string:
            return mariadb_string_literal_size_hint(value.text().size());
        case db_value_type::signed_value:
        case db_value_type::unsigned_value:
            return 32;
        case db_value_type::double_value:
            return 64;
        case db_value_type::bool_value:
            return 1;
    }
    return 0;
}

void append_value_literal(
    std::string& output, const db_value& value, bool backslash_escapes, const MARIADB_CHARSET_INFO* charset) {
    switch (value.type()) {
        case db_value_type::null:
            output.append("NULL");
            break;
        case db_value_type::string:
            append_string_literal(output, value.text(), backslash_escapes, charset);
            break;
        case db_value_type::signed_value:
            append_db_number(output, value.signed_value());
            break;
        case db_value_type::unsigned_value:
            append_db_number(output, value.unsigned_value());
            break;
        case db_value_type::double_value:
            append_db_double_literal(output, value.double_value(), db_driver::mariadb);
            break;
        case db_value_type::bool_value:
            output.push_back(value.bool_value() ? '1' : '0');
            break;
    }
}

// A server error after which the server ends the session, or which shows
// that it read a packet it did not expect.
[[nodiscard]] bool session_ended(unsigned int native_code) noexcept {
    switch (native_code) {
        case ER_SERVER_SHUTDOWN:
        case ER_FORCING_CLOSE:
        case ER_ABORTING_CONNECTION:
        case ER_NET_PACKET_TOO_LARGE:
        case ER_NET_READ_ERROR_FROM_PIPE:
        case ER_NET_FCNTL_ERROR:
        case ER_NET_PACKETS_OUT_OF_ORDER:
        case ER_NET_UNCOMPRESS_ERROR:
        case ER_NET_READ_ERROR:
        case ER_NET_READ_INTERRUPTED:
        case ER_NET_ERROR_ON_WRITE:
        case ER_NET_WRITE_INTERRUPTED:
        case ER_NEW_ABORTING_CONNECTION:
        case ER_CONNECTION_KILLED:
            return true;
        default:
            return false;
    }
}

// One statement-level token of MariaDB SQL.
struct sql_token final {
    enum class kind : std::uint8_t {
        end,
        word,
        number,
        placeholder,
        comma,
        open,
        // An executable comment ("/*!" or "/*M!"): the server runs its
        // content as SQL.
        hidden,
        other,
    };

    kind kind_{kind::end};
    std::string_view text_{};
};

[[nodiscard]] constexpr bool sql_word_byte(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
           byte == '_' || byte == '$' || byte >= 0x80;
}

[[nodiscard]] bool sql_keyword(std::string_view word, std::string_view keyword) noexcept {
    return http_ascii_equals_ignore_case(word, keyword);
}

// Reads the token at `index` and moves past it. Whitespace and comments only
// separate tokens; a literal or quoted identifier is one `other` token.
[[nodiscard]] sql_token next_sql_token(std::string_view sql, std::size_t& index, bool backslash_escapes) noexcept {
    using kind = sql_token::kind;
    while (index < sql.size()) {
        const auto start = index;
        const char character = sql[index];
        if (is_sql_whitespace(character)) {
            ++index;
            continue;
        }
        const bool block_comment = character == '/' && sql.substr(index, 2) == "/*";
        if (block_comment || character == '#' || is_mariadb_double_dash_comment(sql, index)) {
            index = skip_sql_atom(sql, index, backslash_escapes);
            if (block_comment && (sql.substr(start, 3) == "/*!" || sql.substr(start, 4) == "/*M!")) {
                return {kind::hidden, sql.substr(start, index - start)};
            }
            continue;
        }
        if (character == '\'' || character == '"' || character == '`') {
            index = skip_sql_atom(sql, index, backslash_escapes);
            return {kind::other, sql.substr(start, index - start)};
        }
        if (sql_word_byte(character)) {
            while (index < sql.size() && sql_word_byte(sql[index])) {
                ++index;
            }
            const bool number = character >= '0' && character <= '9';
            return {number ? kind::number : kind::word, sql.substr(start, index - start)};
        }
        ++index;
        switch (character) {
            case '?':
                return {kind::placeholder, sql.substr(start, 1)};
            case ',':
                return {kind::comma, sql.substr(start, 1)};
            case '(':
                return {kind::open, sql.substr(start, 1)};
            default:
                return {kind::other, sql.substr(start, 1)};
        }
    }
    return {};
}

// The first token past opening parentheses.
[[nodiscard]] sql_token leading_sql_token(std::string_view sql, bool backslash_escapes) noexcept {
    std::size_t index = 0;
    auto token = next_sql_token(sql, index, backslash_escapes);
    while (token.kind_ == sql_token::kind::open) {
        token = next_sql_token(sql, index, backslash_escapes);
    }
    return token;
}

// A placeholder where the text protocol takes only an integer literal, by the
// tokens before it, most recent first: "LIMIT ?", "OFFSET ?", "FETCH FIRST ?",
// "FETCH NEXT ?", "ROWS EXAMINED ?" and the second count of "LIMIT n, ?".
[[nodiscard]] bool integer_placeholder(const std::array<sql_token, 3>& before) noexcept {
    using kind = sql_token::kind;
    if (before[0].kind_ == kind::word) {
        const auto word = before[0].text_;
        return sql_keyword(word, "limit") || sql_keyword(word, "offset") || sql_keyword(word, "first") ||
               sql_keyword(word, "next") || sql_keyword(word, "examined");
    }
    return before[0].kind_ == kind::comma &&
           (before[1].kind_ == kind::placeholder || before[1].kind_ == kind::number) &&
           before[2].kind_ == kind::word && sql_keyword(before[2].text_, "limit");
}

// The traits of `sql` read in one backslash mode; nullopt-like `placeholders`
// lets the caller compare the two readings.
struct sql_reading final {
    mariadb_statement_traits traits_{true, 0};
    std::size_t placeholders_{0};
};

[[nodiscard]] sql_reading read_mariadb_statement(std::string_view sql, bool backslash_escapes) noexcept {
    using kind = sql_token::kind;
    sql_reading reading;
    auto& traits = reading.traits_;
    std::size_t index = 0;
    auto token = next_sql_token(sql, index, backslash_escapes);
    while (token.kind_ == kind::open) {
        token = next_sql_token(sql, index, backslash_escapes);
    }
    if (token.kind_ == kind::hidden ||
        (token.kind_ == kind::word && (sql_keyword(token.text_, "call") || sql_keyword(token.text_, "load") ||
                                          sql_keyword(token.text_, "set") || sql_keyword(token.text_, "use")))) {
        traits.preparable_ = false;
        return reading;
    }
    std::array<sql_token, 3> before{};
    for (; token.kind_ != kind::end; token = next_sql_token(sql, index, backslash_escapes)) {
        if (token.kind_ == kind::hidden || (token.kind_ == kind::word && sql_keyword(token.text_, "returning"))) {
            traits.preparable_ = false;
            return reading;
        }
        if (token.kind_ == kind::placeholder) {
            if (integer_placeholder(before)) {
                if (reading.placeholders_ >= 64) {
                    traits.preparable_ = false;
                    return reading;
                }
                traits.integer_params_ |= std::uint64_t{1} << reading.placeholders_;
            }
            ++reading.placeholders_;
        }
        before = {token, before[0], before[1]};
    }
    return reading;
}

}  // namespace

void validate_mariadb_sql_length(std::size_t length) {
    if (length > (std::numeric_limits<unsigned long>::max)()) {
        throw std::length_error("MariaDB SQL is too large for the client API");
    }
}

db_error mariadb_error(std::string_view operation, unsigned int native_code, const char* sql_state,
    const char* message, db_error::code_type error_code) {
    // A statement that failed because the connection did is an I/O failure,
    // not a statement the server rejected.
    if (error_code == db_error::code_type::statement_failed &&
        (native_code == CR_SERVER_GONE_ERROR || native_code == CR_SERVER_LOST || native_code == CR_ERR_NET_READ ||
            native_code == CR_ERR_NET_WRITE || session_ended(native_code))) {
        error_code = db_error::code_type::io_error;
    }
    const std::string_view state_view =
        (sql_state != nullptr && sql_state[0] != '\0') ? std::string_view(sql_state) : std::string_view{};
    const std::string_view message_view =
        (message != nullptr && message[0] != '\0') ? std::string_view(message) : std::string_view{};
    std::array<char, http_max_unsigned_decimal_size> digits;
    const std::string_view code_view =
        native_code != 0 ? http_format_unsigned(digits, static_cast<std::uint64_t>(native_code)) : std::string_view{};
    std::string error;
    error.reserve(operation.size() + 7U + (code_view.empty() ? 0U : 9U + code_view.size()) +
                  (state_view.empty() ? 0U : 12U + state_view.size()) +
                  (message_view.empty() ? 0U : 2U + message_view.size()));
    error.append(operation);
    error.append(" failed");
    if (!code_view.empty()) {
        error.append(" [errno=");
        error.append(code_view);
        error.push_back(']');
    }
    if (!state_view.empty()) {
        error.append(" [sqlstate=");
        error.append(state_view);
        error.push_back(']');
    }
    if (!message_view.empty()) {
        error.append(": ");
        error.append(message_view);
    }
    return db_error(error_code, db_driver::mariadb, error,
        native_code == 0 ? std::nullopt : std::optional<std::int64_t>(native_code),
        sql_state == nullptr ? std::string_view{} : std::string_view(sql_state));
}

db_error mysql_error(const st_mysql& connection, std::string_view operation, db_error::code_type error_code) {
    auto* mutable_connection = const_cast<st_mysql*>(&connection);
    return mariadb_error(operation, mysql_errno(mutable_connection), mysql_sqlstate(mutable_connection),
        mysql_error(mutable_connection), error_code);
}

bool mariadb_connection_failure(unsigned int native_code) noexcept {
    // libmariadb's own errors (the CR_ range and its extended CER_ range), and
    // a failure without a code, which has no known cause.
    return native_code == 0 || (native_code >= CR_MIN_ERROR && native_code <= CR_MAX_ERROR) ||
           (native_code >= CER_MIN_ERROR && native_code <= CER_MAX_ERROR) || session_ended(native_code);
}

void free_stored_result(void* result_value) noexcept {
    mysql_free_result(static_cast<st_mysql_res*>(result_value));
}

mariadb_parameter_traits inspect_mariadb_parameters(std::span<const db_value> params) noexcept {
    mariadb_parameter_traits result{};
    for (const auto& param : params) {
        if (param.type() != db_value_type::string) {
            continue;
        }
        const auto value = param.text();
        for (auto backslash = value.find('\\'); backslash != std::string_view::npos;
            backslash = value.find('\\', backslash + 1)) {
            result.has_backslash_ = true;
            if (backslash != 0 && static_cast<unsigned char>(value[backslash - 1]) > 0x7f) {
                result.has_ambiguous_backslash_ = true;
                return result;
            }
        }
    }
    return result;
}

bool mariadb_escape_needs_charset(const st_mysql& connection, bool has_ambiguous_backslash) noexcept {
    return has_ambiguous_backslash && (connection.server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES) == 0;
}

const MARIADB_CHARSET_INFO& mariadb_queried_charset(MYSQL_RES& result) {
    auto* row = mysql_fetch_row(&result);
    const auto* lengths = row == nullptr ? nullptr : mysql_fetch_lengths(&result);
    if (row == nullptr || row[0] == nullptr || lengths == nullptr || mysql_num_fields(&result) != 1) {
        throw std::invalid_argument("MariaDB did not name the session's character set");
    }
    const std::string name(row[0], lengths[0]);
    const auto* charset = mariadb_get_charset_by_name(name.c_str());
    if (charset == nullptr) {
        throw std::invalid_argument(
            (std::string("MariaDB session character set ").append(name).append(" is unknown to the client library, so a string parameter with a backslash "
                                                                               "cannot be escaped for it"))
                .c_str());
    }
    return *charset;
}

const MARIADB_CHARSET_INFO& mariadb_utf8mb4_charset() noexcept {
    // libmariadb's table of character sets is static and always has it.
    static const MARIADB_CHARSET_INFO& charset = *mariadb_get_charset_by_name("utf8mb4");
    return charset;
}

void interpolate_sql(st_mysql& connection, std::string_view sql, std::span<const db_value> params,
    std::string& output, const MARIADB_CHARSET_INFO* charset) {
    validate_mariadb_sql_length(sql.size());
    const bool backslash_escapes = (connection.server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES) == 0;
    std::size_t size_hint = sql.size();
    static constexpr std::size_t inline_placeholder_capacity = 32;
    std::array<std::size_t, inline_placeholder_capacity> inline_placeholders;
    std::size_t scan_offset = 0;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        const auto placeholder = find_sql_syntax_byte(sql, '?', backslash_escapes, scan_offset);
        if (placeholder == std::string_view::npos) {
            throw std::invalid_argument("SQL parameter count does not match placeholders");
        }
        if (i < inline_placeholder_capacity) {
            inline_placeholders[i] = placeholder;
        }
        scan_offset = placeholder + 1;
        if (param.type() == db_value_type::string) {
            const auto value = param.text();
            if (value.find('\0') != std::string_view::npos) {
                throw std::invalid_argument("MariaDB string parameter must not contain NUL bytes");
            }
            if (backslash_escapes && charset == nullptr && ambiguous_backslash(value)) {
                throw std::logic_error(
                    "MariaDB string parameter needs the session's character set to be escaped");
            }
        }
        const auto literal_size_hint = value_literal_size_hint(param);
        if (size_hint > (std::numeric_limits<std::size_t>::max)() - literal_size_hint) {
            throw std::length_error("MariaDB SQL size calculation overflowed");
        }
        size_hint += literal_size_hint;
        validate_mariadb_sql_length(size_hint);
    }
    if (find_sql_syntax_byte(sql, '?', backslash_escapes, scan_offset) != std::string_view::npos) {
        throw std::invalid_argument("SQL parameter count does not match placeholders");
    }
    output.clear();
    output.reserve(size_hint);

    std::size_t offset = 0;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto placeholder = i < inline_placeholder_capacity
                                     ? inline_placeholders[i]
                                     : find_sql_syntax_byte(sql, '?', backslash_escapes, offset);
        output.append(sql.data() + offset, placeholder - offset);
        append_value_literal(output, params[i], backslash_escapes, charset);
        offset = placeholder + 1;
    }
    output.append(sql.data() + offset, sql.size() - offset);
    validate_mariadb_sql_length(output.size());
}

mariadb_statement_traits mariadb_statement_traits_of(std::string_view sql) noexcept {
    // A backslash ends a literal in one mode and escapes in the other; a
    // statement whose two readings disagree on its placeholders is not
    // prepared, and either reading's integer placeholders count.
    const auto escaped = read_mariadb_statement(sql, true);
    if (sql.find('\\') == std::string_view::npos) {
        return escaped.traits_;
    }
    const auto plain = read_mariadb_statement(sql, false);
    if (!escaped.traits_.preparable_ || !plain.traits_.preparable_ || escaped.placeholders_ != plain.placeholders_) {
        return {false, 0};
    }
    return {true, escaped.traits_.integer_params_ | plain.traits_.integer_params_};
}

mariadb_statement_classification classify_mariadb_statement(std::string_view sql) noexcept {
    mariadb_statement_classification result{};
    const auto leading = leading_sql_token(sql, true);
    if (leading.kind_ == sql_token::kind::hidden) {
        result.exclusive_ = true;
        result.calls_routine_ = true;
    } else if (leading.kind_ == sql_token::kind::word) {
        result.exclusive_ = sql_keyword(leading.text_, "load");
        result.calls_routine_ = sql_keyword(leading.text_, "call");
    }
    constexpr std::string_view tracking_name = "session_track";
    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char ch = sql[i];
        if (ch == '\\') {
            result.has_backslash_ = true;
            if (result.names_session_tracking_) {
                break;
            }
        } else if (ch == '_' && !result.names_session_tracking_ && i >= 7 &&
                   sql.size() - (i - 7) >= tracking_name.size() &&
                   http_ascii_equals_ignore_case(sql.substr(i - 7, tracking_name.size()), tracking_name)) {
            result.names_session_tracking_ = true;
            if (result.has_backslash_) {
                break;
            }
        }
    }
    return result;
}

}  // namespace ruvia::detail
