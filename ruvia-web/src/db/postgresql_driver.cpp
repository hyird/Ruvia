#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/error.hpp>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_utils.h"
#include "ruvia/web/detail/integration/request_io_metrics.h"

#include "db/db_pool.h"
#include "db/db_postgresql.h"
#include "db/postgresql_pipeline.h"

namespace ruvia::detail {
namespace {

using postgresql_pool = db_pool<postgresql_driver>;

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

    void reset(PGresult* result_value = nullptr) noexcept {
        if (result_ != nullptr) {
            PQclear(result_);
        }
        result_ = result_value;
    }

private:
    PGresult* result_;
};

[[nodiscard]] bool successful_result_status(ExecStatusType status) noexcept {
    return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK || status == PGRES_EMPTY_QUERY;
}

void materialize_borrowed_result(db_rows& output, PGresult& result_value, std::pmr::memory_resource* resource) {
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
        rows.push_back(db_result_access::borrowed_row(
            fields_value.data() + row_start, field_count, column_names.data(), column_names.size()));
    }
}

[[nodiscard]] db_row materialize_stream_single_row(PGresult& result_value,
    std::pmr::vector<db_field>& fields_value, std::pmr::vector<std::pmr::string>& column_names,
    std::pmr::memory_resource* resource) {
    const auto field_count = static_cast<std::size_t>(PQnfields(&result_value));
    if (column_names.empty()) {
        column_names.reserve(field_count);
        for (std::size_t field = 0; field < field_count; ++field) {
            column_names.emplace_back(PQfname(&result_value, static_cast<int>(field)));
        }
    }
    fields_value.clear();
    fields_value.reserve(field_count);
    for (std::size_t field = 0; field < field_count; ++field) {
        const auto field_index = static_cast<int>(field);
        if (PQgetisnull(&result_value, 0, field_index) != 0) {
            fields_value.push_back(db_result_access::null_field(resource));
            continue;
        }
        fields_value.push_back(db_result_access::owned_field(
            std::string_view(PQgetvalue(&result_value, 0, field_index),
                static_cast<std::size_t>(PQgetlength(&result_value, 0, field_index))),
            resource));
    }
    return db_result_access::borrowed_row(
        fields_value.data(), fields_value.size(), column_names.data(), column_names.size());
}

[[nodiscard]] std::optional<std::chrono::milliseconds> earlier_timeout(
    std::optional<std::chrono::milliseconds> left, std::optional<std::chrono::milliseconds> right) noexcept {
    if (!left.has_value()) {
        return right;
    }
    if (!right.has_value()) {
        return left;
    }
    return std::min(*left, *right);
}

[[nodiscard]] bool flush_postgresql(pg_conn& connection) {
    const auto status = PQflush(&connection);
    if (status < 0) {
        throw postgresql_error(connection, "PQflush", db_error::code_type::io_error);
    }
    return status != 0;
}

// db_config's read and write timeouts bound waiting without socket progress in
// that direction; waiting while output is pending waits to write.
[[nodiscard]] std::optional<std::chrono::milliseconds> postgresql_io_timeout(
    const db_config_storage& config, db_socket_readiness wanted) noexcept {
    return has_db_socket_readiness(wanted, db_socket_readiness::write) ? config.write_timeout_ : config.read_timeout_;
}

// One readiness wait on the connection's current socket.
[[nodiscard]] postgresql_pool::slot_wait wait_postgresql_socket(postgresql_pool& pool, std::size_t index,
    db_socket_readiness wanted, std::optional<std::chrono::milliseconds> wait_timeout,
    const operation_timeout& deadline_value) {
    const auto native = PQsocket(pool.slot(index).connection_);
    if (native < 0) {
        throw db_error(
            db_error::code_type::io_error, db_driver::postgresql, "PostgreSQL connection socket is unavailable");
    }
    return pool.wait_socket(
        index, static_cast<db_slot_socket::native_socket_type>(native), wanted, wait_timeout, deadline_value);
}

// A socket error ends the connection. A cancelled wait does not: it expired,
// or it was woken to be armed again.
void check_postgresql_wait(const db_socket_wait_result& result) {
    if (result.error_ && result.error_ != asio::error::operation_aborted) {
        throw db_error(db_error::code_type::io_error, db_driver::postgresql,
            std::string("PostgreSQL socket wait failed: ").append(error_code_text(result.error_)), result.error_.value());
    }
}

// After a readiness wake: reads what arrived and, while output is pending,
// sends what libpq still holds. PQconsumeInput() never blocks, so it is
// harmless after a write-readiness wake, and draining input keeps a server
// blocked on sending results from never reading our query. True while output
// remains.
[[nodiscard]] bool exchange_postgresql_io(pg_conn& connection, bool output_pending) {
    if (PQconsumeInput(&connection) == 0) {
        throw postgresql_error(connection, "PQconsumeInput", db_error::code_type::io_error);
    }
    return output_pending && flush_postgresql(connection);
}

// One socket wait of a statement that holds its connection alone, bounded by
// the statement deadline and the I/O timeout of its direction. Expiry fails
// the operation like its overall deadline does: libpq has no per-I/O timeout
// to hand the wait back to.
struct postgresql_socket_wait final {
    postgresql_pool& pool_;
    postgresql_pool::slot_wait wait_;

    [[nodiscard]] bool await_ready() {
        return wait_.await_ready();
    }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept {
        return wait_.await_suspend(handle);
    }
    void await_resume() {
        const auto wait_result = wait_.await_resume();
        if (wait_result.expired_) {
            throw pool_.timed_out();
        }
        check_postgresql_wait(wait_result);
    }
};

[[nodiscard]] postgresql_socket_wait wait_for_postgresql(postgresql_pool& pool, std::size_t index,
    db_socket_readiness wanted, const operation_timeout& deadline_value) {
    const auto remaining = deadline_value.remaining();
    if (remaining.has_value() && remaining->count() <= 0) {
        throw pool.timed_out();
    }
    return postgresql_socket_wait{pool,
        wait_postgresql_socket(pool, index, wanted,
            earlier_timeout(remaining, postgresql_io_timeout(pool.config(), wanted)), deadline_value)};
}

// Called only after PQflush() reports pending output: waits for read or write
// readiness until the output is sent.
task<void> flush_output(postgresql_pool& pool, std::size_t index, const operation_timeout& deadline_value) {
    auto& connection = *pool.slot(index).connection_;
    do {
        co_await wait_for_postgresql(
            pool, index, db_socket_readiness::read | db_socket_readiness::write, deadline_value);
    } while (exchange_postgresql_io(connection, true));
}

task<void> wait_until_result_ready(postgresql_pool& pool, std::size_t index, const operation_timeout& deadline_value) {
    auto* connection = pool.slot(index).connection_;
    while (PQisBusy(connection) != 0) {
        co_await wait_for_postgresql(pool, index, db_socket_readiness::read, deadline_value);
        (void)exchange_postgresql_io(*connection, false);
    }
}

void validate_postgresql_statement(std::string_view sql, std::span<const db_value> params) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if (sql.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SQL must not contain NUL bytes");
    }
    if (!std::in_range<int>(params.size())) {
        throw std::invalid_argument("too many PostgreSQL query parameters");
    }
}

// A stream runs as the unnamed statement: libpq selects single-row mode only
// for the command at the head of its queue, which a Parse sent ahead of it
// would occupy. `sql` is NUL-terminated (db_pool's hook contract), so it
// reaches libpq without a copy. Sending and the first flush are synchronous;
// only pending output needs a flush_output task. The extended protocol
// preserves the server-enforced single-statement boundary even without
// parameters.
[[nodiscard]] bool send_stream_query(
    postgresql_pool& pool, std::size_t index, std::string_view sql, std::span<const db_value> params) {
    validate_postgresql_statement(sql, params);
    auto* connection = pool.slot(index).connection_;
    auto encoded = encode_postgresql_params(params, pool.resource());
    const auto* values = encoded.values_.empty() ? nullptr : encoded.values_.data();
    const auto* types = encoded.types_.empty() ? nullptr : encoded.types_.data();
    if (PQsendQueryParams(
            connection, sql.data(), static_cast<int>(params.size()), types, values, nullptr, nullptr, 0) == 0) {
        throw postgresql_error(*connection, "PQsendQueryParams", db_error::code_type::statement_failed);
    }
    if (PQsetSingleRowMode(connection) == 0) {
        throw postgresql_error(*connection, "PQsetSingleRowMode", db_error::code_type::statement_failed);
    }
    return flush_postgresql(*connection);
}

// Queues the commands of one buffered statement flight and its Sync on a
// connection in pipeline mode. Each failure leaves the connection outside its
// idle protocol state, so it is an I/O failure and the connection is closed,
// which also discards its cache entries. The Close ahead of Parse lets a name
// the server may still hold (a statement forgotten as stale) parse again.
[[nodiscard]] postgresql_flight queue_cached_statement(pg_conn& connection,
    const postgresql_statement_cache& statements, const postgresql_statement_cache::lookup& lookup,
    std::string_view sql, const postgresql_params& encoded) {
    const auto count = static_cast<int>(encoded.values_.size());
    const auto* values = encoded.values_.empty() ? nullptr : encoded.values_.data();
    const auto* types = encoded.types_.empty() ? nullptr : encoded.types_.data();
    const auto queue_failure = [&connection](std::string_view operation) {
        return postgresql_error(connection, operation, db_error::code_type::io_error);
    };
    postgresql_flight flight{lookup.statement_, 0, lookup.inserted_};
    if (!lookup.statement_.has_value()) {
        if (PQsendQueryParams(&connection, sql.data(), count, types, values, nullptr, nullptr, 0) == 0) {
            throw queue_failure("PQsendQueryParams");
        }
    } else {
        const auto* name = statements.name(*lookup.statement_);
        if (lookup.evicted_.has_value()) {
            if (PQsendClosePrepared(&connection, lookup.evicted_->data()) == 0) {
                throw queue_failure("PQsendClosePrepared");
            }
            ++flight.setup_commands_;
        }
        if (lookup.inserted_) {
            if (PQsendClosePrepared(&connection, name) == 0) {
                throw queue_failure("PQsendClosePrepared");
            }
            if (PQsendPrepare(&connection, name, sql.data(), count, types) == 0) {
                throw queue_failure("PQsendPrepare");
            }
            flight.setup_commands_ += 2;
        }
        if (PQsendQueryPrepared(&connection, name, count, values, nullptr, nullptr, 0) == 0) {
            throw queue_failure("PQsendQueryPrepared");
        }
    }
    if (PQsendPipelineSync(&connection) == 0) {
        throw queue_failure("PQsendPipelineSync");
    }
    return flight;
}

[[nodiscard]] std::string_view postgresql_error_field(const PGresult& result_value, int field) noexcept {
    const char* value = PQresultErrorField(&result_value, field);
    return value == nullptr ? std::string_view{} : std::string_view(value);
}

// A cached statement the server no longer holds (26000), or whose result type
// changed under its cached plan (0A000), is prepared again on its next use.
[[nodiscard]] bool postgresql_stale_statement(const PGresult& result_value) noexcept {
    const auto state = postgresql_error_field(result_value, PG_DIAG_SQLSTATE);
    return state == "26000" || state == "0A000";
}

// Whether the server reported a stale statement while looking it up or
// revalidating its plan for Bind -- before it ran -- rather than an error the
// statement raised itself with the same SQLSTATE.
[[nodiscard]] bool postgresql_stale_before_execution(const PGresult& result_value) noexcept {
    const auto state = postgresql_error_field(result_value, PG_DIAG_SQLSTATE);
    const auto routine = postgresql_error_field(result_value, PG_DIAG_SOURCE_FUNCTION);
    return (state == "26000" && routine == "FetchPreparedStatement") ||
           (state == "0A000" && routine == "RevalidateCachedQuery");
}

[[nodiscard]] db_error postgresql_protocol_failure(std::string_view message) {
    return db_error(db_error::code_type::protocol_error, db_driver::postgresql, std::string(message));
}

[[nodiscard]] db_error postgresql_cancelled() {
    return db_error(db_error::code_type::cancelled, db_driver::postgresql, "database operation cancelled");
}

// Feeds the next PQgetResult() of a flight into its results; true once the
// flight's Sync arrived. Rows go to `output` unless it is null (execute(), or
// a statement its caller abandoned). A failure that leaves no protocol
// boundary (transport loss, COPY, an unexpected result) is thrown, and the
// connection must close; an error the server reported for the flight is
// recorded and the rest of the flight read up to its Sync.
[[nodiscard]] bool consume_flight_result(pg_conn& connection, const postgresql_flight& flight,
    postgresql_flight_results& results, PGresult* next_result, bool returns_rows, db_rows* output,
    std::pmr::memory_resource* resource) {
    const std::string_view operation = returns_rows ? "PostgreSQL query" : "PostgreSQL execute";
    postgresql_result_owner result_value(next_result);
    if (result_value.get() == nullptr) {
        if (++results.command_ > flight.setup_commands_ + 1) {
            throw postgresql_protocol_failure("PostgreSQL pipeline ended without its Sync");
        }
        return false;
    }
    const auto status = PQresultStatus(result_value.get());
    if (status == PGRES_PIPELINE_SYNC) {
        if (results.command_ != flight.setup_commands_ + 1) {
            throw postgresql_protocol_failure("PostgreSQL pipeline Sync arrived early");
        }
        return true;
    }
    if (status == PGRES_PIPELINE_ABORTED) {
        return false;
    }
    if (status == PGRES_FATAL_ERROR) {
        if (PQstatus(&connection) == CONNECTION_BAD) {
            throw postgresql_error(connection, operation, db_error::code_type::io_error, result_value.get());
        }
        if (!results.failure_.has_value()) {
            results.statement_failed_ = results.command_ == flight.setup_commands_;
            results.stale_ = postgresql_stale_statement(*result_value);
            results.stale_before_execution_ = postgresql_stale_before_execution(*result_value);
            results.failure_.emplace(
                postgresql_error(connection, operation, db_error::code_type::statement_failed, result_value.get()));
        }
        return false;
    }
    if (results.command_ < flight.setup_commands_) {
        if (status != PGRES_COMMAND_OK) {
            throw postgresql_protocol_failure("PostgreSQL statement setup returned an unexpected result");
        }
        results.parsed_ = results.parsed_ || (flight.parses_ && results.command_ + 1 == flight.setup_commands_);
        return false;
    }
    if (results.command_ != flight.setup_commands_ ||
        (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK && status != PGRES_EMPTY_QUERY)) {
        throw postgresql_protocol_failure("PostgreSQL statement returned an unsupported result status");
    }
    if (status != PGRES_TUPLES_OK) {
        results.affected_rows_ = postgresql_affected_rows(*result_value);
        return false;
    }
    if (results.returned_rows_) {
        throw postgresql_protocol_failure("PostgreSQL returned multiple tuple results");
    }
    results.returned_rows_ = true;
    if (returns_rows && output != nullptr && results.output_failure_ == nullptr) {
        // The result is complete, so a failure to keep it (an allocation in
        // the caller's resource) leaves the protocol in step: the flight is
        // read on to its Sync and only this statement fails.
        try {
            materialize_borrowed_result(*output, *result_value, resource);
            db_result_access::own_raw_result(*output, result_value.release(), &free_postgresql_result);
        } catch (...) {
            db_result_access::rows(*output).clear();
            db_result_access::fields(*output).clear();
            db_result_access::column_names(*output).clear();
            results.output_failure_ = std::current_exception();
        }
    }
    return false;
}

enum class postgresql_flight_end : std::uint8_t {
    completed,
    failed,
    output_failed,
    rows_required,
    rows_refused,
    retry,
};

// Settles a flight read up to its Sync. A Parse that failed leaves no server
// statement, and a stale cached statement is forgotten so its next use parses
// it again. A stale statement that failed before running, outside a
// transaction block, runs once more. The whole statement is read before a row
// set is refused, so a shape mismatch leaves the connection idle.
[[nodiscard]] postgresql_flight_end finish_flight(postgresql_statement_cache& statements,
    const postgresql_flight& flight, const postgresql_flight_results& results, bool returns_rows, bool may_retry,
    bool outside_transaction) noexcept {
    if (flight.parses_ && !results.parsed_) {
        statements.forget(*flight.statement_);
    }
    if (results.output_failure_ != nullptr) {
        return postgresql_flight_end::output_failed;
    }
    if (results.failure_.has_value()) {
        if (results.statement_failed_ && flight.statement_.has_value() && !flight.parses_ && results.stale_) {
            statements.forget(*flight.statement_);
            if (may_retry && results.stale_before_execution_ && outside_transaction) {
                return postgresql_flight_end::retry;
            }
        }
        return postgresql_flight_end::failed;
    }
    if (returns_rows != results.returned_rows_) {
        return returns_rows ? postgresql_flight_end::rows_required : postgresql_flight_end::rows_refused;
    }
    return postgresql_flight_end::completed;
}

[[nodiscard]] std::exception_ptr flight_failure(postgresql_flight_end end, postgresql_flight_results& results) {
    switch (end) {
        case postgresql_flight_end::failed:
            return std::make_exception_ptr(std::move(*results.failure_));
        case postgresql_flight_end::output_failed:
            return results.output_failure_;
        case postgresql_flight_end::rows_required:
            return std::make_exception_ptr(std::invalid_argument("query() requires row-producing SQL"));
        case postgresql_flight_end::rows_refused:
            return std::make_exception_ptr(std::invalid_argument("execute() does not accept row-producing SQL"));
        case postgresql_flight_end::completed:
        case postgresql_flight_end::retry:
            break;
    }
    std::terminate();
}

// Runs one buffered statement on a connection its caller holds exclusively (a
// transaction's statements and control commands): the connection enters
// pipeline mode for the statement's single round trip and leaves it once the
// whole flight is read, so it is idle again whenever the statement itself
// failed.
template <typename result_type>
task<result_type> run_cached_statement(postgresql_pool& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    constexpr bool returns_rows = std::is_same_v<result_type, db_rows>;
    validate_postgresql_statement(sql, params);
    const auto encoded = encode_postgresql_params(params, pool.resource());
    auto* connection = pool.slot(index).connection_;
    auto& statements = pool.slot(index).session_.statements_;
    for (bool may_retry = true;; may_retry = false) {
        const auto lookup = statements.find_or_insert(sql, encoded.types_);
        if (PQenterPipelineMode(connection) == 0) {
            throw postgresql_error(*connection, "PQenterPipelineMode", db_error::code_type::io_error);
        }
        const auto flight = queue_cached_statement(*connection, statements, lookup, sql, encoded);
        if (flush_postgresql(*connection)) {
            co_await flush_output(pool, index, deadline_value);
        }
        auto output = db_result_access::make_result(resource);
        postgresql_flight_results results;
        while (true) {
            // Keep readiness in this frame, including the final protocol boundary.
            while (PQisBusy(connection) != 0) {
                co_await wait_for_postgresql(pool, index, db_socket_readiness::read, deadline_value);
                (void)exchange_postgresql_io(*connection, false);
            }
            if (consume_flight_result(
                    *connection, flight, results, PQgetResult(connection), returns_rows, &output, resource)) {
                break;
            }
        }
        if (PQexitPipelineMode(connection) == 0) {
            throw postgresql_error(*connection, "PQexitPipelineMode", db_error::code_type::protocol_error);
        }
        const auto end = finish_flight(
            statements, flight, results, returns_rows, may_retry, PQtransactionStatus(connection) == PQTRANS_IDLE);
        if (end == postgresql_flight_end::retry) {
            continue;
        }
        if (end != postgresql_flight_end::completed) {
            std::rethrow_exception(flight_failure(end, results));
        }
        if constexpr (returns_rows) {
            co_return output;
        } else {
            co_return db_result_access::make_exec_result(results.affected_rows_);
        }
    }
}

// Reads the group's results for one statement while it holds the reader role,
// from taking the role to the statement's outcome. A stop or the statement's
// deadline abandons the statement, which passes the role on as it leaves; any
// other failure while reading fails the connection for every member.
class postgresql_pipeline_reader final {
public:
    using clock_type = std::chrono::steady_clock;

    postgresql_pipeline_reader(postgresql_pool& pool, postgresql_pipeline_member& member) noexcept
        : pool_(pool),
          member_(member) {}

    postgresql_pipeline_reader(const postgresql_pipeline_reader&) = delete;
    postgresql_pipeline_reader& operator=(const postgresql_pipeline_reader&) = delete;

    // One socket wait; a failure of the wait itself fails the connection.
    struct socket_wait final {
        postgresql_pipeline_reader& reader_;
        postgresql_pool::slot_wait wait_;

        [[nodiscard]] bool await_ready() {
            try {
                return wait_.await_ready();
            } catch (...) {
                reader_.fail(std::current_exception());
            }
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept {
            return wait_.await_suspend(handle);
        }
        void await_resume() {
            try {
                reader_.waited(wait_.await_resume());
            } catch (...) {
                reader_.fail(std::current_exception());
            }
        }
    };

    // Reads what libpq holds; true once the statement has its outcome.
    [[nodiscard]] bool read() {
        if (member_.stopped()) {
            throw postgresql_cancelled();
        }
        const auto now = clock_type::now();
        if (member_.deadline_.has_value() && *member_.deadline_ <= now) {
            throw pool_.timed_out();
        }
        auto& state = pool_.state();
        state.expire_followers(member_, now);
        try {
            return state.read_buffered(member_);
        } catch (...) {
            fail(std::current_exception());
        }
    }

    // Waits for results, and to write while output is pending, until the
    // earliest deadline of a pending member or the I/O timeout of the
    // direction waited for, counted from the connection's last progress.
    [[nodiscard]] socket_wait wait() {
        try {
            const auto slot = member_.slot_;
            auto& group = pool_.slot(slot).session_.pipeline_;
            const auto wanted = group.flush_pending_ ? db_socket_readiness::read | db_socket_readiness::write
                                                     : db_socket_readiness::read;
            const auto now = clock_type::now();
            if (!group.stretch_start_.has_value()) {
                group.stretch_start_ = now;
            }
            io_deadline_.reset();
            if (const auto io_timeout = postgresql_io_timeout(pool_.config(), wanted)) {
                io_deadline_ = worker_timer_saturating_deadline(
                    *group.stretch_start_, worker_timer_saturating_duration_cast(*io_timeout));
            }
            auto until = pool_.state().earliest_deadline(slot);
            if (io_deadline_.has_value() && (!until.has_value() || *io_deadline_ < *until)) {
                until = io_deadline_;
            }
            std::optional<std::chrono::milliseconds> wait_timeout;
            if (until.has_value()) {
                wait_timeout = worker_timer_ceil_milliseconds(*until - now);
            }
            group.wait_deadline_ = until;
            return socket_wait{*this, wait_postgresql_socket(pool_, slot, wanted, wait_timeout, unbounded_)};
        } catch (...) {
            fail(std::current_exception());
        }
    }

private:
    // A cancelled wait (a stop, a member joining with an earlier deadline or
    // output to send) returns to read(), which looks again.
    void waited(const db_socket_wait_result& result) {
        auto& slot = pool_.slot(member_.slot_);
        auto& group = slot.session_.pipeline_;
        group.wait_deadline_.reset();
        check_postgresql_wait(result);
        if (result.ready_ != db_socket_readiness::none) {
            group.stretch_start_.reset();
            group.flush_pending_ = exchange_postgresql_io(*slot.connection_, group.flush_pending_);
        } else if (result.expired_ && io_deadline_.has_value() && clock_type::now() >= *io_deadline_) {
            throw pool_.timed_out();
        }
    }

    [[noreturn]] void fail(const std::exception_ptr& failure) {
        pool_.state().fail_group(member_.slot_, failure);
        if (member_.stopped()) {
            throw postgresql_cancelled();
        }
        std::rethrow_exception(member_.failure_ != nullptr ? member_.failure_ : failure);
    }

    postgresql_pool& pool_;
    postgresql_pipeline_member& member_;
    const operation_timeout unbounded_{std::nullopt};
    std::optional<clock_type::time_point> io_deadline_{};
};

// One buffered statement outside a transaction, as a member of a pipeline
// group (see postgresql_pipeline.h): admission, a connection of its own when
// it founds a group, its flight, then its outcome -- read by itself while it
// holds the reader role, else handed over by the reader. Every wait is an
// awaiter on this frame, so a statement costs no coroutine frame beyond this
// one unless it connects. The member's destructor gives back whatever it
// holds on every exit path.
template <typename result_type>
task<result_type> run_pipelined(postgresql_pool& pool, std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options, operation_scope* scope) {
    constexpr bool returns_rows = std::is_same_v<result_type, db_rows>;
    scoped_request_io_timer timer(scope, request_io_kind::db);
    validate_postgresql_statement(sql, params);
    const auto encoded = encode_postgresql_params(params, pool.resource());
    const operation_timeout timeout(options.timeout_);
    auto& pipeline = pool.state();
    auto output = db_result_access::make_result(resource);
    postgresql_pipeline_member member(pipeline, returns_rows ? &output : nullptr, resource, options.stop_token_);

    // Waiting for admission is bounded like acquiring a lease.
    if (pipeline.admit(member, timeout.constrained_by(pool.config().acquire_timeout_)) ==
        postgresql_member_event::none) {
        pipeline.admitted(co_await postgresql_pipeline_member::wait{member});
    }
    if (member.phase_ == postgresql_member_phase::leased) {
        if (pipeline.must_connect(member)) {
            std::exception_ptr failure;
            try {
                co_await pool.connect_slot(member.slot_, timeout);
            } catch (...) {
                failure = std::current_exception();
            }
            pipeline.connected(member, failure);
        }
        pipeline.found_group(member);
    }

    member.deadline_ = timeout.constrained_by(pool.config().query_timeout_).deadline();
    for (;;) {
        pipeline.send(member, sql, encoded);
        auto event = postgresql_member_event::none;
        while (event == postgresql_member_event::none) {
            if (pipeline.reads(member)) {
                postgresql_pipeline_reader reader(pool, member);
                while (!reader.read()) {
                    co_await reader.wait();
                }
                event = member.take_event();
            } else if ((event = co_await postgresql_pipeline_member::wait{member}) ==
                       postgresql_member_event::reader) {
                event = postgresql_member_event::none;
            }
        }
        if (!pipeline.retries(member, event)) {
            break;
        }
    }
    if constexpr (returns_rows) {
        co_return output;
    } else {
        co_return db_result_access::make_exec_result(member.affected_rows_);
    }
}

[[nodiscard]] operation_timeout postgresql_stream_deadline(
    const postgresql_pool& pool, const operation_options& options) {
    return operation_timeout(options.timeout_).constrained_by(pool.config().query_timeout_);
}

}  // namespace

postgresql_pipeline_member::postgresql_pipeline_member(postgresql_pipeline_state& state, db_rows* rows,
    std::pmr::memory_resource* resource, const stop_token& stop_token_value)
    : state_(state),
      rows_(rows),
      resource_(resource),
      stop_token_(stop_token_value),
      cancellation_(state.cancellation_target(), cancellation_id_) {
    cancellation_.arm(stop_token_);
    // Linked once binding cannot fail; a stop delivered while binding is seen
    // through stopped(). Newest first: a stop source runs its newest callback
    // first, so stopping every statement at once (a client closing) finds each
    // one at the front.
    next_ = state_.members_;
    if (next_ != nullptr) {
        next_->previous_ = this;
    }
    state_.members_ = this;
}

postgresql_pipeline_member::~postgresql_pipeline_member() {
    state_.retire(*this);
}

postgresql_pipeline_state::postgresql_pipeline_state(pool_type& pool, const worker_handle& worker_value)
    : pool_(pool),
      worker_(worker_value),
      cancellation_target_(make_worker_cancellation_target(*this, worker_value)) {}

postgresql_pipeline_state::~postgresql_pipeline_state() {
    cancellation_target_->detach(*this);
    // Started statements complete before their pool is destroyed.
    if (members_ != nullptr || parked_head_ != nullptr) {
        std::terminate();
    }
}

postgresql_pipeline_group& postgresql_pipeline_state::group(std::size_t slot) const noexcept {
    return pool_.slot(slot).session_.pipeline_;
}

bool postgresql_pipeline_state::may_share(std::uint64_t arrival) const noexcept {
    const auto oldest = pool_.oldest_exclusive_arrival();
    return !oldest.has_value() || arrival < *oldest;
}

// A resume from inside the reader, a stop callback or a timer would re-enter
// the woken statement; it is deferred to the worker instead. Taking the
// continuation makes each suspension resumable exactly once.
void postgresql_pipeline_state::settle(postgresql_pipeline_member& member, postgresql_member_event event) noexcept {
    member.event_ = event;
    if (auto continuation = std::exchange(member.continuation_, {})) {
        worker_.defer_resume(continuation);
    }
}

std::optional<std::size_t> postgresql_pipeline_state::roomiest_group() const noexcept {
    std::optional<std::size_t> roomiest;
    std::size_t lowest_load = postgresql_pipeline_depth;
    for (std::size_t index = 0; index < pool_.config().pool_size_per_worker_; ++index) {
        const auto& candidate = group(index);
        const auto load = candidate.pending_ + candidate.reserved_;
        if (candidate.members_ != 0 && !candidate.closing_ && !candidate.stalled() && pool_.slot(index).connected_ &&
            load < lowest_load) {
            roomiest = index;
            lowest_load = load;
        }
    }
    return roomiest;
}

void postgresql_pipeline_state::join(postgresql_pipeline_member& member, std::size_t slot) noexcept {
    auto& joined = group(slot);
    ++joined.members_;
    ++joined.reserved_;
    member.slot_ = slot;
    member.phase_ = postgresql_member_phase::reserved;
}

postgresql_member_event postgresql_pipeline_state::place(postgresql_pipeline_member& member) {
    if (!may_share(member.arrival_)) {
        return postgresql_member_event::none;
    }
    if (const auto slot = pool_.try_lease()) {
        member.slot_ = *slot;
        member.phase_ = postgresql_member_phase::leased;
        return postgresql_member_event::leased;
    }
    if (const auto slot = roomiest_group()) {
        join(member, *slot);
        return postgresql_member_event::joined;
    }
    return postgresql_member_event::none;
}

// Parked statements go first, so an arrival only places itself when nobody
// is parked.
postgresql_member_event postgresql_pipeline_state::admit(
    postgresql_pipeline_member& member, const operation_timeout& admission_deadline) {
    if (pool_.closing()) {
        throw pool_.closing_error();
    }
    if (member.stopped()) {
        throw postgresql_cancelled();
    }
    member.arrival_ = pool_.next_arrival();
    if (parked_head_ == nullptr) {
        if (const auto event = place(member); event != postgresql_member_event::none) {
            return event;
        }
    }
    park(member, admission_deadline);
    return postgresql_member_event::none;
}

void postgresql_pipeline_state::admitted(postgresql_member_event event) const {
    switch (event) {
        case postgresql_member_event::leased:
        case postgresql_member_event::joined:
            return;
        case postgresql_member_event::timed_out:
            pool_type::slot_policy::throw_acquire_failure(pool_, worker_wait_status::timed_out);
        case postgresql_member_event::cancelled:
            throw postgresql_cancelled();
        case postgresql_member_event::closing:
            throw pool_.closing_error();
        default:
            std::terminate();
    }
}

void postgresql_pipeline_state::park(postgresql_pipeline_member& member, const operation_timeout& admission_deadline) {
    if (const auto deadline_value = admission_deadline.deadline()) {
        if (*deadline_value <= std::chrono::steady_clock::now()) {
            pool_type::slot_policy::throw_acquire_failure(pool_, worker_wait_status::timed_out);
        }
        // A stopped timer queue arms nothing; the statement still leaves
        // through admission, its stop token or close().
        (void)worker_.schedule_timer(
            member.admission_timer_, *deadline_value, [this, &member](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired && member.phase_ == postgresql_member_phase::parked) {
                    unpark(member);
                    member.phase_ = postgresql_member_phase::admitting;
                    settle(member, postgresql_member_event::timed_out);
                }
            });
    }
    member.previous_parked_ = parked_tail_;
    member.next_parked_ = nullptr;
    if (parked_tail_ != nullptr) {
        parked_tail_->next_parked_ = &member;
    } else {
        parked_head_ = &member;
    }
    parked_tail_ = &member;
    member.phase_ = postgresql_member_phase::parked;
}

// Leaves the phase to the caller, which knows where the statement goes next.
void postgresql_pipeline_state::unpark(postgresql_pipeline_member& member) noexcept {
    member.admission_timer_.cancel();
    if (member.previous_parked_ != nullptr) {
        member.previous_parked_->next_parked_ = member.next_parked_;
    } else {
        parked_head_ = member.next_parked_;
    }
    if (member.next_parked_ != nullptr) {
        member.next_parked_->previous_parked_ = member.previous_parked_;
    } else {
        parked_tail_ = member.previous_parked_;
    }
    member.previous_parked_ = nullptr;
    member.next_parked_ = nullptr;
}

void postgresql_pipeline_state::dispatch() noexcept {
    while (parked_head_ != nullptr && !pool_.closing()) {
        auto& head = *parked_head_;
        const auto event = place(head);
        if (event == postgresql_member_event::none) {
            return;
        }
        unpark(head);
        settle(head, event);
    }
}

// A released connection goes to the oldest parked statement unless an older
// transaction or stream waits for it in the lease scheduler. Room would have
// admitted that statement already.
bool postgresql_pipeline_state::take_released_slot(std::size_t slot) noexcept {
    if (parked_head_ == nullptr || pool_.closing() || !may_share(parked_head_->arrival_)) {
        return false;
    }
    auto& head = *parked_head_;
    unpark(head);
    head.slot_ = slot;
    head.phase_ = postgresql_member_phase::leased;
    settle(head, postgresql_member_event::leased);
    return true;
}

void postgresql_pipeline_state::exclusive_waiter_left() noexcept {
    dispatch();
}

void postgresql_pipeline_state::close() noexcept {
    while (parked_head_ != nullptr) {
        auto& head = *parked_head_;
        unpark(head);
        head.phase_ = postgresql_member_phase::admitting;
        settle(head, postgresql_member_event::closing);
    }
}

bool postgresql_pipeline_state::must_connect(postgresql_pipeline_member& member) {
    pool_.reset_abort(member.slot_);
    if (member.stopped()) {
        throw postgresql_cancelled();
    }
    member.connecting_ = !pool_.slot(member.slot_).connected_;
    return member.connecting_;
}

// A stop while connecting closed the slot, which ended the connect.
void postgresql_pipeline_state::connected(postgresql_pipeline_member& member, const std::exception_ptr& failure) {
    member.connecting_ = false;
    if (failure == nullptr) {
        return;
    }
    if (member.stopped()) {
        throw postgresql_cancelled();
    }
    std::rethrow_exception(failure);
}

void postgresql_pipeline_state::found_group(postgresql_pipeline_member& member) {
    const auto index = member.slot_;
    auto& slot = pool_.slot(index);
    if (PQenterPipelineMode(slot.connection_) == 0) {
        auto failure = postgresql_error(*slot.connection_, "PQenterPipelineMode", db_error::code_type::io_error);
        pool_.close_slot(index);
        throw failure;
    }
    auto& founded = slot.session_.pipeline_;
    founded.members_ = 1;
    founded.reserved_ = 1;
    member.phase_ = postgresql_member_phase::reserved;
    dispatch();
}

void postgresql_pipeline_state::require_usable(std::size_t slot) const {
    const auto& checked = group(slot);
    if (checked.closing_) {
        std::rethrow_exception(checked.failure_);
    }
    pool_.throw_if_aborted(slot);
    if (!pool_.slot(slot).connected_) {
        throw pool_.closing_error();
    }
}

// Preparing the flight (the cache lookup) changes nothing on the wire, so its
// failure is the statement's own; a failure while queueing it leaves a partial
// flight and fails the connection.
void postgresql_pipeline_state::send(
    postgresql_pipeline_member& member, std::string_view sql, const postgresql_params& encoded) {
    if (member.stopped()) {
        throw postgresql_cancelled();
    }
    if (member.deadline_.has_value() && *member.deadline_ <= std::chrono::steady_clock::now()) {
        throw pool_.timed_out();
    }
    const auto index = member.slot_;
    auto& slot = pool_.slot(index);
    auto& sending = slot.session_.pipeline_;
    require_usable(index);
    auto& statements = slot.session_.statements_;
    const auto lookup = statements.find_or_insert(sql, encoded.types_);
    auto* connection = slot.connection_;
    postgresql_flight flight;
    bool flush_pending = false;
    try {
        flight = queue_cached_statement(*connection, statements, lookup, sql, encoded);
        flush_pending = flush_postgresql(*connection);
    } catch (...) {
        fail_group(index, std::current_exception());
        throw;
    }
    auto& record = sending.records_[(sending.head_ + sending.pending_) % postgresql_pipeline_depth];
    record = {&member, flight, member.rows_ != nullptr};
    ++sending.pending_;
    --sending.reserved_;
    member.record_ = &record;
    member.phase_ = postgresql_member_phase::pending;
    sending.flush_pending_ = sending.flush_pending_ || flush_pending;
    if (sending.reader_ == nullptr) {
        sending.reader_ = &member;
        return;
    }
    if (!slot.wait_active_) {
        return;
    }
    // The waiting reader looks again when output waits to be sent, when this
    // statement's deadline comes first, or when sending read results into
    // libpq (a full socket makes libpq read while it writes).
    const bool earlier = member.deadline_.has_value() &&
                         (!sending.wait_deadline_.has_value() || *member.deadline_ < *sending.wait_deadline_);
    if (flush_pending || earlier || PQisBusy(connection) == 0) {
        slot.wait_socket_.cancel();
    }
}

bool postgresql_pipeline_state::reads(const postgresql_pipeline_member& member) const noexcept {
    return group(member.slot_).reader_ == &member;
}

bool postgresql_pipeline_state::retries(postgresql_pipeline_member& member, postgresql_member_event event) const {
    switch (event) {
        case postgresql_member_event::completed:
            if (member.failure_ != nullptr) {
                std::rethrow_exception(member.failure_);
            }
            return false;
        case postgresql_member_event::retry:
            return true;
        case postgresql_member_event::failed:
            std::rethrow_exception(member.failure_);
        case postgresql_member_event::timed_out:
            throw pool_.timed_out();
        case postgresql_member_event::cancelled:
            throw postgresql_cancelled();
        default:
            std::terminate();
    }
}

void postgresql_pipeline_state::read_result(std::size_t slot, postgresql_pipeline_member& reader) {
    auto& reading = group(slot);
    auto* connection = pool_.slot(slot).connection_;
    auto& record = reading.records_[reading.head_];
    auto* member = record.member_;
    if (consume_flight_result(*connection, record.flight_, reading.head_results_, PQgetResult(connection),
            record.returns_rows_, member == nullptr ? nullptr : member->rows_,
            member == nullptr ? nullptr : member->resource_)) {
        complete_head(slot, reader);
    }
}

// Hands the head flight's outcome to its member, which the reader settles
// itself and wakes any other; an abandoned flight's outcome is discarded.
void postgresql_pipeline_state::complete_head(std::size_t slot, postgresql_pipeline_member& reader) {
    auto& connection_slot = pool_.slot(slot);
    auto& completing = connection_slot.session_.pipeline_;
    auto& record = completing.records_[completing.head_];
    completing.stretch_start_.reset();
    auto* member = record.member_;
    // libpq reports a transaction status only once nothing follows in its
    // queue; later flights make it PQTRANS_ACTIVE. A shared statement never
    // runs inside a transaction block, so a status libpq cannot tell yet is
    // outside one.
    const auto status = PQtransactionStatus(connection_slot.connection_);
    const auto end = finish_flight(connection_slot.session_.statements_, record.flight_, completing.head_results_,
        record.returns_rows_, member != nullptr && member->may_retry_,
        status == PQTRANS_IDLE || status == PQTRANS_ACTIVE);
    auto results = std::move(completing.head_results_);
    completing.head_results_ = {};
    record = {};
    completing.head_ = (completing.head_ + 1) % postgresql_pipeline_depth;
    --completing.pending_;
    if (member == nullptr) {
        dispatch();
        return;
    }
    member->record_ = nullptr;
    auto event = postgresql_member_event::completed;
    if (end == postgresql_flight_end::retry) {
        // The retry keeps the room it frees, and goes to the tail.
        member->may_retry_ = false;
        member->phase_ = postgresql_member_phase::reserved;
        ++completing.reserved_;
        event = postgresql_member_event::retry;
    } else {
        member->phase_ = postgresql_member_phase::settled;
        if (end == postgresql_flight_end::completed) {
            member->affected_rows_ = results.affected_rows_;
        } else {
            member->failure_ = flight_failure(end, results);
        }
    }
    if (member == &reader) {
        member->event_ = event;
    } else {
        settle(*member, event);
    }
    if (event != postgresql_member_event::retry) {
        dispatch();
    }
}

bool postgresql_pipeline_state::read_buffered(postgresql_pipeline_member& reader) {
    const auto slot = reader.slot_;
    require_usable(slot);
    auto* connection = pool_.slot(slot).connection_;
    while (reader.event_ == postgresql_member_event::none) {
        if (PQisBusy(connection) != 0) {
            return false;
        }
        read_result(slot, reader);
    }
    // Results libpq already holds reach their members before the role passes
    // on; a failure among them no longer concerns the reader's own outcome.
    auto& reading = group(slot);
    try {
        while (reading.pending_ != 0 && PQisBusy(connection) == 0) {
            read_result(slot, reader);
        }
    } catch (...) {
        fail_group(slot, std::current_exception());
    }
    hand_off(reader);
    return true;
}

void postgresql_pipeline_state::expire_followers(
    postgresql_pipeline_member& reader, std::chrono::steady_clock::time_point now) noexcept {
    auto& reading = group(reader.slot_);
    for (std::size_t offset = 0; offset < reading.pending_; ++offset) {
        auto* member = reading.records_[(reading.head_ + offset) % postgresql_pipeline_depth].member_;
        if (member != nullptr && member != &reader && member->event_ == postgresql_member_event::none &&
            member->deadline_.has_value() && *member->deadline_ <= now) {
            abandon(*member);
            settle(*member, postgresql_member_event::timed_out);
        }
    }
}

std::optional<std::chrono::steady_clock::time_point> postgresql_pipeline_state::earliest_deadline(
    std::size_t slot) const noexcept {
    const auto& reading = group(slot);
    std::optional<std::chrono::steady_clock::time_point> earliest;
    for (std::size_t offset = 0; offset < reading.pending_; ++offset) {
        const auto* member = reading.records_[(reading.head_ + offset) % postgresql_pipeline_depth].member_;
        if (member != nullptr && member->event_ == postgresql_member_event::none && member->deadline_.has_value() &&
            (!earliest.has_value() || *member->deadline_ < *earliest)) {
            earliest = member->deadline_;
        }
    }
    return earliest;
}

void postgresql_pipeline_state::hand_off(postgresql_pipeline_member& reader) noexcept {
    auto& reading = group(reader.slot_);
    if (reading.reader_ != &reader) {
        return;
    }
    reading.reader_ = nullptr;
    for (std::size_t offset = 0; offset < reading.pending_; ++offset) {
        auto* member = reading.records_[(reading.head_ + offset) % postgresql_pipeline_depth].member_;
        if (member != nullptr && member != &reader && member->event_ == postgresql_member_event::none) {
            reading.reader_ = member;
            settle(*member, postgresql_member_event::reader);
            return;
        }
    }
}

void postgresql_pipeline_state::abandon(postgresql_pipeline_member& member) noexcept {
    if (member.record_ != nullptr) {
        member.record_->member_ = nullptr;
        member.record_ = nullptr;
    }
    member.phase_ = postgresql_member_phase::settled;
}

void postgresql_pipeline_state::clear_flights(postgresql_pipeline_group& cleared) noexcept {
    for (std::size_t offset = 0; offset < cleared.pending_; ++offset) {
        cleared.records_[(cleared.head_ + offset) % postgresql_pipeline_depth] = {};
    }
    cleared.head_results_ = {};
    cleared.wait_deadline_.reset();
    cleared.stretch_start_.reset();
    cleared.reader_ = nullptr;
    cleared.head_ = 0;
    cleared.pending_ = 0;
    cleared.flush_pending_ = false;
}

// The first failure is the group's; a designated reader that has not resumed
// yet learns it instead of its role.
void postgresql_pipeline_state::fail_group(std::size_t slot, const std::exception_ptr& failure) noexcept {
    auto& failing = group(slot);
    if (!failing.closing_) {
        failing.closing_ = true;
        failing.failure_ = failure;
    }
    for (std::size_t offset = 0; offset < failing.pending_; ++offset) {
        auto* member = failing.records_[(failing.head_ + offset) % postgresql_pipeline_depth].member_;
        if (member == nullptr) {
            continue;
        }
        member->record_ = nullptr;
        member->phase_ = postgresql_member_phase::settled;
        if (member->event_ == postgresql_member_event::none || member->event_ == postgresql_member_event::reader) {
            member->failure_ = failing.failure_;
            settle(*member, postgresql_member_event::failed);
        }
    }
    clear_flights(failing);
    pool_.close_slot(slot);
}

void postgresql_pipeline_state::unlink(postgresql_pipeline_member& member) noexcept {
    if (member.previous_ != nullptr) {
        member.previous_->next_ = member.next_;
    } else {
        members_ = member.next_;
    }
    if (member.next_ != nullptr) {
        member.next_->previous_ = member.previous_;
    }
    member.previous_ = nullptr;
    member.next_ = nullptr;
}

// A cancelled statement leaves the search, so stopping every statement at once
// costs one step each.
void postgresql_pipeline_state::cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id == 0) {
        return;
    }
    auto* member = members_;
    while (member != nullptr && member->cancellation_id_ != cancellation_id) {
        member = member->next_;
    }
    if (member == nullptr) {
        return;
    }
    unlink(*member);
    member->cancel_requested_ = true;
    switch (member->phase_) {
        case postgresql_member_phase::parked:
            unpark(*member);
            member->phase_ = postgresql_member_phase::admitting;
            settle(*member, postgresql_member_event::cancelled);
            return;
        case postgresql_member_phase::leased:
            if (member->connecting_) {
                pool_.close_slot(member->slot_);
            }
            return;
        case postgresql_member_phase::pending: {
            auto& slot = pool_.slot(member->slot_);
            if (slot.session_.pipeline_.reader_ == member) {
                // The reader leaves its wait and passes the role on.
                if (slot.wait_active_) {
                    slot.wait_socket_.cancel();
                }
            } else if (member->event_ == postgresql_member_event::none) {
                abandon(*member);
                settle(*member, postgresql_member_event::cancelled);
            }
            return;
        }
        // Running, or about to: it sees stopped() at its next step.
        case postgresql_member_phase::admitting:
        case postgresql_member_phase::reserved:
        case postgresql_member_phase::settled:
            return;
    }
}

void postgresql_pipeline_state::retire(postgresql_pipeline_member& member) noexcept {
    if (!member.cancel_requested_) {
        unlink(member);
    }
    switch (member.phase_) {
        case postgresql_member_phase::admitting:
            break;
        case postgresql_member_phase::parked:
            unpark(member);
            break;
        case postgresql_member_phase::leased:
            member.phase_ = postgresql_member_phase::admitting;
            pool_.release_slot(member.slot_);
            break;
        case postgresql_member_phase::reserved:
            --group(member.slot_).reserved_;
            leave(member);
            break;
        case postgresql_member_phase::pending:
            hand_off(member);
            abandon(member);
            leave(member);
            break;
        case postgresql_member_phase::settled:
            hand_off(member);
            leave(member);
            break;
    }
}

void postgresql_pipeline_state::leave(postgresql_pipeline_member& member) noexcept {
    member.phase_ = postgresql_member_phase::admitting;
    if (--group(member.slot_).members_ != 0) {
        dispatch();
        return;
    }
    dissolve(member.slot_);
}

// The last member gives the connection back. Flights abandoned before their
// results arrived leave results nobody reads, so that connection is closed,
// as is one that failed, cannot leave pipeline mode, or was left inside a
// transaction block by a statement such as BEGIN.
void postgresql_pipeline_state::dissolve(std::size_t slot) noexcept {
    auto& connection_slot = pool_.slot(slot);
    auto& dissolving = connection_slot.session_.pipeline_;
    auto* connection = connection_slot.connection_;
    if (dissolving.closing_ || dissolving.pending_ != 0 || !connection_slot.connected_ ||
        PQexitPipelineMode(connection) == 0 || PQtransactionStatus(connection) != PQTRANS_IDLE) {
        pool_.close_slot(slot);
    }
    clear_flights(dissolving);
    dissolving.failure_ = nullptr;
    dissolving.reserved_ = 0;
    dissolving.closing_ = false;
    pool_.release_slot(slot);
}

task<void> postgresql_driver::connect(pool_type& pool, std::size_t index, const operation_timeout& deadline_value) {
    const auto& config = pool.config();
    auto& slot = pool.slot(index);
    slot.session_.statements_.clear();
    auto addresses = co_await pool.resolve_host(index, deadline_value);
    auto resolved_hosts = make_postgresql_resolved_host_list(config.tls_.tls_host(config.host_), addresses, pool.resource());
    const auto port = format_db_port(config.port_);
    // Pin the client encoding to UTF-8. Ruvia's strings are UTF-8 throughout,
    // and query parameters are sent in text format; without this the connection
    // inherits the server/database default encoding, so non-ASCII parameters and
    // result text would be misinterpreted on a non-UTF-8 database (e.g. LATIN1,
    // SQL_ASCII). libpq accepts client_encoding as a connection keyword.
    const bool use_tls = config.tls_.mode_ == client_tls_mode::verify_identity;
    const std::array<const char*, 14> keywords{"host", "hostaddr", "port", "user", "password", "dbname",
        "client_encoding", "sslmode", "sslrootcert", "sslcert", "sslkey", "gssencmode", "ssl_min_protocol_version",
        nullptr};
    const std::array<const char*, 14> values{resolved_hosts.hosts_.c_str(), resolved_hosts.addresses_.c_str(),
        port.data(), config.username_.c_str(), config.password_.c_str(), config.database_.c_str(), "UTF8",
        use_tls ? "verify-full" : "disable",
        use_tls ? (config.tls_.ca_file_.empty() ? "system" : config.tls_.ca_file_.c_str()) : "",
        config.tls_.certificate_chain_file_.c_str(), config.tls_.private_key_file_.c_str(), "disable", "TLSv1.2",
        nullptr};
    slot.connection_ = PQconnectStartParams(keywords.data(), values.data(), 0);
    if (slot.connection_ == nullptr) {
        throw db_error(db_error::code_type::connect_failed, db_driver::postgresql, "PQconnectStartParams failed");
    }
    if (PQstatus(slot.connection_) == CONNECTION_BAD) {
        throw postgresql_error(*slot.connection_, "PQconnectStartParams", db_error::code_type::connect_failed);
    }

    // PQconnectStartParams() has already polled once and left the socket
    // connect pending, so the first PQconnectPoll() must wait for writability;
    // polling earlier reads no socket error yet and loses the fallback to the
    // next resolved address when that connect fails.
    auto status = PGRES_POLLING_WRITING;
    while (status == PGRES_POLLING_READING || status == PGRES_POLLING_WRITING || status == PGRES_POLLING_ACTIVE) {
        if (status != PGRES_POLLING_ACTIVE) {
            co_await wait_for_postgresql(pool, index,
                status == PGRES_POLLING_READING ? db_socket_readiness::read : db_socket_readiness::write,
                deadline_value);
        }
        status = PQconnectPoll(slot.connection_);
    }
    if (status != PGRES_POLLING_OK || PQstatus(slot.connection_) != CONNECTION_OK) {
        throw postgresql_error(*slot.connection_, "PQconnectPoll", db_error::code_type::connect_failed);
    }
    if (use_tls && PQsslInUse(slot.connection_) != 1) {
        throw db_error(db_error::code_type::connect_failed, db_driver::postgresql, "PostgreSQL refused required TLS");
    }
    if (PQsetnonblocking(slot.connection_, 1) != 0) {
        throw postgresql_error(*slot.connection_, "PQsetnonblocking", db_error::code_type::connect_failed);
    }
}

void postgresql_driver::close_connection(connection_type& connection) noexcept {
    PQfinish(&connection);
}

task<db_rows> postgresql_driver::query(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    return run_cached_statement<db_rows>(pool, index, sql, params, resource, deadline_value);
}

task<db_exec_result> postgresql_driver::execute(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    return run_cached_statement<db_exec_result>(pool, index, sql, params, resource, deadline_value);
}

task<db_rows> postgresql_driver::query_pipelined(pool_type& pool, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource, operation_options options,
    operation_scope* scope) {
    return run_pipelined<db_rows>(pool, std::move(sql), std::move(params), resource, std::move(options), scope);
}

task<db_exec_result> postgresql_driver::execute_pipelined(pool_type& pool, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource, operation_options options,
    operation_scope* scope) {
    return run_pipelined<db_exec_result>(pool, std::move(sql), std::move(params), resource, std::move(options), scope);
}

// Single-row mode reports whether the statement produced rows only with its
// first result, so the stream starts optimistically and read_stream_row()
// refuses a statement without a row set.
task<db_stream_start> postgresql_driver::start_stream(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    if (send_stream_query(pool, index, sql, params)) {
        co_await flush_output(pool, index, deadline_value);
    }
    co_return db_stream_start(resource);
}

task<std::optional<db_row>> postgresql_driver::read_stream_row(pool_type& pool, std::size_t index, void*,
    std::pmr::vector<db_field>& fields, std::pmr::vector<std::pmr::string>& column_names,
    std::pmr::memory_resource* resource, const operation_options& options) {
    auto* connection = pool.slot(index).connection_;
    auto cancellation = pool.cancel_on_stop(index, options.stop_token_);
    bool slot_released = false;
    try {
        pool.throw_if_aborted(index);
        const auto deadline_value = postgresql_stream_deadline(pool, options);
        co_await wait_until_result_ready(pool, index, deadline_value);
        postgresql_result_owner result_value(PQgetResult(connection));
        if (result_value.get() == nullptr) {
            slot_released = true;
            cancellation.finish();
            pool.release_slot(index);
            co_return std::nullopt;
        }
        const auto status = PQresultStatus(result_value.get());
        if (status == PGRES_SINGLE_TUPLE) {
            co_return materialize_stream_single_row(*result_value, fields, column_names, resource);
        }
        if (status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK || status == PGRES_EMPTY_QUERY) {
            result_value.reset();
            while (true) {
                co_await wait_until_result_ready(pool, index, deadline_value);
                postgresql_result_owner remaining(PQgetResult(connection));
                if (remaining.get() == nullptr) {
                    break;
                }
                if (!successful_result_status(PQresultStatus(remaining.get()))) {
                    throw postgresql_error(
                        *connection, "PostgreSQL stream", db_error::code_type::statement_failed, remaining.get());
                }
            }
            slot_released = true;
            cancellation.finish();
            pool.release_slot(index);
            if (status == PGRES_COMMAND_OK || status == PGRES_EMPTY_QUERY) {
                throw std::invalid_argument("query_stream() requires row-producing SQL");
            }
            co_return std::nullopt;
        }
        throw postgresql_error(
            *connection, "PostgreSQL stream", db_error::code_type::statement_failed, result_value.get());
    } catch (...) {
        if (!slot_released) {
            pool.close_slot(index);
            cancellation.finish();
            pool.release_slot(index);
        }
        throw;
    }
}

task<void> postgresql_driver::close_stream(
    pool_type& pool, std::size_t index, void*, std::pmr::memory_resource*, const operation_options& options) {
    auto cancellation = pool.cancel_on_stop(index, options.stop_token_);
    // Abandoning a libpq single-row result still requires draining the whole
    // command. Closing is bounded and keeps the worker non-blocking.
    pool.close_slot(index);
    cancellation.finish();
    pool.release_slot(index);
    if (options.stop_token_.stop_requested()) {
        throw db_error(db_error::code_type::cancelled, db_driver::postgresql, "database operation cancelled");
    }
    co_return;
}

void postgresql_driver::discard_stream(void*) noexcept {}

}  // namespace ruvia::detail
