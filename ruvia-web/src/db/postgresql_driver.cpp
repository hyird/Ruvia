#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/url_encoding.h"

#include "db/db_pipeline_impl.h"
#include "db/db_pool.h"
#include "db/db_sql_format.h"
#include "db/postgresql_pipeline.h"
#include "http/operation_interrupt.h"
namespace ruvia::detail {
namespace {

using postgresql_pool = db_pool<postgresql_driver>;

// pg_type OID of float8 (double precision); libpq's headers name no type OIDs.
constexpr unsigned int postgresql_float8_oid = 701;

void validate_postgresql_string_param(std::string_view value) {
    if (value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("PostgreSQL string parameter must not contain NUL bytes");
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("PostgreSQL parameter is too large");
    }
}

void validate_postgresql_double_param(double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("database double value must be finite");
    }
}

void populate_postgresql_column_names(
    std::vector<std::string>& column_names, PGresult& result_value, std::size_t field_count) {
    for (std::size_t field = 0; field < field_count; ++field) {
        column_names.emplace_back(PQfname(&result_value, static_cast<int>(field)));
    }
}

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

void materialize_borrowed_result(db_rows& output, PGresult& result_value) {
    const auto row_count = static_cast<std::size_t>(PQntuples(&result_value));
    const auto field_count = static_cast<std::size_t>(PQnfields(&result_value));
    auto& fields_value = db_result_access::fields(output);
    auto& column_names = db_result_access::column_names(output);
    db_result_access::reserve_result(output, row_count, field_count);
    populate_postgresql_column_names(column_names, result_value, field_count);
    for (std::size_t row = 0; row < row_count; ++row) {
        const auto row_start = fields_value.size();
        for (std::size_t field = 0; field < field_count; ++field) {
            const auto row_index = static_cast<int>(row);
            const auto field_index = static_cast<int>(field);
            if (PQgetisnull(&result_value, row_index, field_index) != 0) {
                fields_value.push_back(db_result_access::null_field());
                continue;
            }
            fields_value.push_back(db_result_access::borrowed_field(
                std::string_view(PQgetvalue(&result_value, row_index, field_index),
                    static_cast<std::size_t>(PQgetlength(&result_value, row_index, field_index)))));
        }
        db_result_access::append_row(output, row_start, field_count);
    }
}

[[nodiscard]] db_row materialize_stream_single_row(PGresult& result_value,
    std::vector<db_field>& fields_value, std::vector<std::string>& column_names) {
    const auto field_count = static_cast<std::size_t>(PQnfields(&result_value));
    if (column_names.empty()) {
        column_names.reserve(field_count);
        populate_postgresql_column_names(column_names, result_value, field_count);
    }
    return db_result_access::materialize_stream_row(
        fields_value, column_names, field_count, [&](std::size_t field) -> std::optional<std::string_view> {
            const auto field_index = static_cast<int>(field);
            if (PQgetisnull(&result_value, 0, field_index) != 0) {
                return std::nullopt;
            }
            return std::string_view(PQgetvalue(&result_value, 0, field_index),
                static_cast<std::size_t>(PQgetlength(&result_value, 0, field_index)));
        });
}

[[nodiscard]] bool flush_postgresql(pg_conn& connection) {
    const auto status = PQflush(&connection);
    if (status < 0) {
        throw postgresql_error(connection, "PQflush", db_error::code_type::io_error);
    }
    return status != 0;
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
[[nodiscard]] db_driver_socket_wait<postgresql_driver> wait_for_postgresql(postgresql_pool& pool,
    std::size_t index, db_socket_readiness wanted, const operation_deadline& deadline_value) {
    if (deadline_value.expired()) {
        throw pool.timed_out();
    }
    return db_driver_socket_wait<postgresql_driver>{pool,
        pool.wait_socket(index, postgresql_pipeline_io::socket(pool, index), wanted,
            deadline_value.constrained_by(db_io_timeout(pool.config(), wanted)).deadline(), deadline_value)};
}

// Called only after PQflush() reports pending output: waits for read or write
// readiness until the output is sent.
task<void> flush_output(postgresql_pool& pool, std::size_t index, const operation_deadline& deadline_value) {
    auto& connection = *pool.slot(index).connection_;
    do {
        co_await wait_for_postgresql(
            pool, index, db_socket_readiness::read | db_socket_readiness::write, deadline_value);
    } while (exchange_postgresql_io(connection, true));
}

task<void> wait_until_result_ready(postgresql_pool& pool, std::size_t index, const operation_deadline& deadline_value) {
    auto* connection = pool.slot(index).connection_;
    while (PQisBusy(connection) != 0) {
        co_await wait_for_postgresql(pool, index, db_socket_readiness::read, deadline_value);
        (void)exchange_postgresql_io(*connection, false);
    }
}

// A stream runs as the unnamed statement: libpq selects single-row mode only
// for the command at the head of its queue, which a Parse sent ahead of it
// would occupy. `sql` is NUL-terminated (db_pool's hook contract), so it
// reaches libpq without a copy. Sending and the first flush are synchronous;
// only pending output needs a flush_output task. The extended protocol
// preserves the server-enforced single-statement boundary even without
// parameters. Use a pipeline Sync even though the stream owns the connection:
// advancing past the command resets libpq's single-row mode before reuse.
// A non-pipeline stream followed by PQenterPipelineMode leaves that mode set.
[[nodiscard]] bool send_stream_query(
    postgresql_pool& pool, std::size_t index, std::string_view sql, std::span<const db_value> params) {
    auto* connection = pool.slot(index).connection_;
    const postgresql_params encoded(params);
    if (PQenterPipelineMode(connection) == 0) {
        throw postgresql_error(*connection, "PQenterPipelineMode", db_error::code_type::io_error);
    }
    if (PQsendQueryParams(
            connection, sql.data(), encoded.count(), encoded.types(), encoded.values(), nullptr, nullptr, 0) == 0) {
        throw postgresql_error(*connection, "PQsendQueryParams", db_error::code_type::statement_failed);
    }
    if (PQsetSingleRowMode(connection) == 0) {
        throw postgresql_error(*connection, "PQsetSingleRowMode", db_error::code_type::statement_failed);
    }
    if (PQsendPipelineSync(connection) == 0) {
        throw postgresql_error(*connection, "PQsendPipelineSync", db_error::code_type::io_error);
    }
    return flush_postgresql(*connection);
}

// Queues the commands of one buffered statement flight and its Sync on a
// connection in pipeline mode. Each failure leaves the connection outside its
// idle protocol state, so it is an I/O failure and the connection is closed,
// which also discards its cache entries. The Close ahead of Parse lets a name
// the server may still hold (a statement forgotten as stale) parse again.
[[nodiscard]] postgresql_flight queue_cached_statement(pg_conn& connection,
    postgresql_statement_cache& statements, std::string_view sql, const postgresql_params& encoded) {
    const auto lookup = statements.find(sql, encoded.type_span(), true, &make_postgresql_statement_name);
    const auto count = encoded.count();
    const auto* values = encoded.values();
    const auto* types = encoded.types();
    const auto queue_failure = [&connection](std::string_view operation) {
        return postgresql_error(connection, operation, db_error::code_type::io_error);
    };
    postgresql_flight flight{lookup.statement_, 0, lookup.inserted_};
    if (!lookup.statement_.has_value()) {
        if (PQsendQueryParams(&connection, sql.data(), count, types, values, nullptr, nullptr, 0) == 0) {
            throw queue_failure("PQsendQueryParams");
        }
    } else {
        const auto* name = statements.payload(*lookup.statement_)->data();
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
    return db_error(db_error::code_type::protocol_error, db_driver::postgresql, message);
}

// Feeds the next PQgetResult() of a flight into its results; true once the
// flight's Sync arrived. Rows go to `output` unless it is null (execute(), or
// a statement its caller abandoned). A failure that leaves no protocol
// boundary (transport loss, COPY, an unexpected result) is thrown, and the
// connection must close; an error the server reported for the flight is
// recorded and the rest of the flight read up to its Sync.
[[nodiscard]] bool consume_flight_result(pg_conn& connection, const postgresql_flight& flight,
    postgresql_flight_results& results, PGresult* next_result, bool returns_rows, db_rows* output) {
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
            materialize_borrowed_result(*output, *result_value);
            db_result_access::own_raw_result(*output, result_value.release(), &free_postgresql_result);
        } catch (...) {
            db_result_access::clear(*output);
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
        (void)statements.forget(*flight.statement_, false);
    }
    if (results.output_failure_ != nullptr) {
        return postgresql_flight_end::output_failed;
    }
    if (results.failure_.has_value()) {
        if (results.statement_failed_ && flight.statement_.has_value() && !flight.parses_ && results.stale_) {
            (void)statements.forget(*flight.statement_, false);
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
    std::span<const db_value> params, const operation_deadline& deadline_value) {
    constexpr bool returns_rows = std::is_same_v<result_type, db_rows>;
    const postgresql_params encoded(params);
    auto* connection = pool.slot(index).connection_;
    auto& statements = pool.slot(index).session_.statements_;
    for (bool may_retry = true;; may_retry = false) {
        if (PQenterPipelineMode(connection) == 0) {
            throw postgresql_error(*connection, "PQenterPipelineMode", db_error::code_type::io_error);
        }
        const auto flight = queue_cached_statement(*connection, statements, sql, encoded);
        if (flush_postgresql(*connection)) {
            co_await flush_output(pool, index, deadline_value);
        }
        auto output = db_result_access::make_result();
        postgresql_flight_results results;
        while (true) {
            // Keep readiness in this frame, including the final protocol boundary.
            while (PQisBusy(connection) != 0) {
                co_await wait_for_postgresql(pool, index, db_socket_readiness::read, deadline_value);
                (void)exchange_postgresql_io(*connection, false);
            }
            if (consume_flight_result(
                    *connection, flight, results, PQgetResult(connection), returns_rows, &output)) {
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

}  // namespace

// The digest covers the SQL, a NUL that SQL never contains, and every type
// OID as four big-endian bytes, so no two keys share an encoding.
postgresql_statement_name make_postgresql_statement_name(
    std::string_view sql, std::span<const unsigned int> types) {
    db_sha256 key;
    key.update(sql);
    key.update(std::string_view("\0", 1));
    for (const auto type : types) {
        const std::array<char, 4> encoded{static_cast<char>((type >> 24) & 0xffU),
            static_cast<char>((type >> 16) & 0xffU), static_cast<char>((type >> 8) & 0xffU),
            static_cast<char>(type & 0xffU)};
        key.update({encoded.data(), encoded.size()});
    }
    const auto digest = key.finish();
    constexpr std::string_view prefix{"ruvia_"};
    postgresql_statement_name name;
    prefix.copy(name.data(), prefix.size());
    http_encode_lower_hex(name.data() + prefix.size(), std::span(digest.data(), (name.size() - prefix.size()) / 2));
    name.back() = '\0';
    return name;
}

db_error postgresql_error(const pg_conn& connection, std::string_view operation,
    db_error::code_type error_code, const pg_result* result_value) {
    // libpq marks the connection bad when the transport fails, and reports
    // that failure as a fatal result; it is an I/O failure, not a statement
    // the server rejected.
    if (error_code == db_error::code_type::statement_failed && PQstatus(&connection) == CONNECTION_BAD) {
        error_code = db_error::code_type::io_error;
    }
    const char* state_value = result_value == nullptr
                                  ? nullptr
                                  : PQresultErrorField(const_cast<PGresult*>(result_value), PG_DIAG_SQLSTATE);
    const char* constraint = result_value == nullptr ? nullptr
                                                     : PQresultErrorField(const_cast<PGresult*>(result_value),
                                                           PG_DIAG_CONSTRAINT_NAME);
    const std::string_view state_view =
        (state_value != nullptr && state_value[0] != '\0') ? std::string_view(state_value) : std::string_view{};
    const char* message = result_value == nullptr ? PQerrorMessage(const_cast<PGconn*>(&connection))
                                                  : PQresultErrorMessage(const_cast<PGresult*>(result_value));
    std::string_view message_view{};
    if (message != nullptr && message[0] != '\0') {
        while (*message == ' ' || *message == '\r' || *message == '\n') {
            ++message;
        }
        message_view = message;
        while (!message_view.empty() && (message_view.back() == '\r' || message_view.back() == '\n')) {
            message_view.remove_suffix(1);
        }
    }
    std::string error;
    error.reserve(operation.size() + 7U + (state_view.empty() ? 0U : 12U + state_view.size()) +
                  (message_view.empty() ? 0U : 2U + message_view.size()));
    error.append(operation);
    error.append(" failed");
    if (!state_view.empty()) {
        error.append(" [sqlstate=");
        error.append(state_view);
        error.push_back(']');
    }
    if (!message_view.empty()) {
        error.append(": ");
        error.append(message_view);
    }
    return db_error(error_code, db_driver::postgresql, error, std::nullopt, state_view,
        constraint == nullptr ? std::string_view{} : std::string_view(constraint));
}

postgresql_params::postgresql_params(std::span<const db_value> params)
    : count_(params.size()),
      heap_encoded_(),
      heap_values_(),
      heap_types_() {
    // libpq counts parameters in an int.
    if (!std::in_range<int>(params.size())) {
        throw std::invalid_argument("too many PostgreSQL query parameters");
    }
    if (params.empty()) {
        return;
    }
    const bool inline_params = params.size() <= inline_capacity;
    std::size_t numeric_count = 0;
    for (const auto& param : params) {
        const auto type = param.type();
        if (type == db_value_type::string) {
            validate_postgresql_string_param(db_value_access::owned_text(param));
        } else if (type == db_value_type::signed_value || type == db_value_type::unsigned_value ||
                   type == db_value_type::double_value) {
            if (type == db_value_type::double_value) {
                validate_postgresql_double_param(param.double_value());
            }
            ++numeric_count;
        }
    }
    if (!inline_params) {
        heap_values_.reserve(params.size());
        heap_types_.reserve(params.size());
    }
    const bool inline_numerics = inline_params || numeric_count <= inline_capacity;
    if (!inline_numerics) {
        heap_encoded_.resize(numeric_count);
    }

    std::size_t numeric_index = 0;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        const auto type = param.type();
        const unsigned int oid = type == db_value_type::double_value ? postgresql_float8_oid : 0U;
        const char* text = nullptr;
        switch (type) {
            case db_value_type::null:
                break;
            case db_value_type::string:
                text = db_value_access::owned_text(param).c_str();
                break;
            case db_value_type::bool_value:
                text = param.bool_value() ? "true" : "false";
                break;
            case db_value_type::signed_value:
            case db_value_type::unsigned_value:
            case db_value_type::double_value: {
                char* const slot = inline_numerics
                                       ? inline_encoded_.data() + numeric_index * numeric_slot_size
                                       : heap_encoded_[numeric_index].data();
                ++numeric_index;
                std::to_chars_result formatted{};
                if (type == db_value_type::signed_value) {
                    formatted = std::to_chars(slot, slot + numeric_slot_size - 1, param.signed_value());
                } else if (type == db_value_type::unsigned_value) {
                    formatted = std::to_chars(slot, slot + numeric_slot_size - 1, param.unsigned_value());
                } else {
                    formatted = std::to_chars(slot, slot + numeric_slot_size - 1, param.double_value());
                }
                if (formatted.ec != std::errc{}) {
                    throw std::logic_error("failed to format PostgreSQL numeric parameter");
                }
                *formatted.ptr = '\0';
                text = slot;
                break;
            }
        }
        if (inline_params) {
            inline_types_[i] = oid;
            inline_values_[i] = text;
        } else {
            heap_types_.push_back(oid);
            heap_values_.push_back(text);
        }
    }
}

std::uint64_t postgresql_affected_rows(const pg_result& result_value) noexcept {
    const auto* text = PQcmdTuples(const_cast<PGresult*>(&result_value));
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    std::uint64_t value = 0;
    const auto* end = text;
    while (*end >= '0' && *end <= '9') {
        ++end;
    }
    const auto parsed_value = std::from_chars(text, end, value);
    return parsed_value.ec == std::errc{} && parsed_value.ptr == end ? value : 0;
}

postgresql_pipeline_io::statement_type postgresql_pipeline_io::prepare(
    std::string_view sql, std::span<const db_value> params) {
    return {sql, params};
}

db_slot_socket::native_socket_type postgresql_pipeline_io::socket(pool_type& pool, std::size_t slot) {
    const auto native = PQsocket(pool.slot(slot).connection_);
    if (native < 0) {
        throw db_error(
            db_error::code_type::io_error, db_driver::postgresql, "PostgreSQL connection socket is unavailable");
    }
    return static_cast<db_slot_socket::native_socket_type>(native);
}

void postgresql_pipeline_io::open(state_type& state, std::size_t slot) {
    auto* connection = state.pool().slot(slot).connection_;
    if (PQenterPipelineMode(connection) == 0) {
        auto failure = postgresql_error(*connection, "PQenterPipelineMode", db_error::code_type::io_error);
        state.pool().close_slot(slot);
        throw failure;
    }
}

// Preparing the flight (the cache lookup) changes nothing on the wire, so its
// failure is the statement's own; a failure while queueing it leaves a partial
// flight and fails the connection. The waiting reader looks again when output
// waits to be sent, or when sending read results into libpq (a full socket
// makes libpq read while it writes).
bool postgresql_pipeline_io::queue(
    state_type& state, std::size_t index, member_type&, statement_type& statement, flight_type& flight) {
    auto& slot = state.pool().slot(index);
    auto* connection = slot.connection_;
    bool flush_pending = false;
    try {
        flight = queue_cached_statement(*connection, slot.session_.statements_, statement.sql_, statement.encoded_);
        flush_pending = flush_postgresql(*connection);
    } catch (...) {
        state.fail_group(index, std::current_exception());
        throw;
    }
    auto& data = slot.session_.pipeline_.io_;
    data.flush_pending_ = data.flush_pending_ || flush_pending;
    return flush_pending || (slot.wait_active_ && PQisBusy(connection) == 0);
}

// Feeds one result libpq holds into the head flight, and hands the flight's
// outcome on once its Sync arrived.
bool postgresql_pipeline_io::advance(state_type& state, std::size_t index, member_type& reader) {
    auto& slot = state.pool().slot(index);
    auto* connection = slot.connection_;
    if (PQisBusy(connection) != 0) {
        return false;
    }
    auto& reading = slot.session_.pipeline_;
    auto& record = reading.records_[reading.head_];
    auto* member = record.member_;
    auto& results = reading.io_.head_results_;
    if (!consume_flight_result(*connection, record.flight_, results, PQgetResult(connection), record.returns_rows_,
            member == nullptr ? nullptr : member->rows_)) {
        return true;
    }
    // libpq reports a transaction status only once nothing follows in its
    // queue; later flights make it PQTRANS_ACTIVE. A shared statement never
    // runs inside a transaction block, so a status libpq cannot tell yet is
    // outside one.
    const auto status = PQtransactionStatus(connection);
    const auto end = finish_flight(slot.session_.statements_, record.flight_, results, record.returns_rows_,
        member != nullptr && member->may_retry_, status == PQTRANS_IDLE || status == PQTRANS_ACTIVE);
    db_pipeline_outcome outcome;
    if (end == postgresql_flight_end::retry) {
        outcome.retry_ = true;
    } else if (end == postgresql_flight_end::completed) {
        outcome.affected_rows_ = results.affected_rows_;
    } else if (member != nullptr) {
        outcome.failure_ = flight_failure(end, results);
    }
    results = {};
    state.complete_head(index, reader, std::move(outcome));
    return true;
}

db_socket_readiness postgresql_pipeline_io::wanted(pool_type& pool, std::size_t slot) noexcept {
    return pool.slot(slot).session_.pipeline_.io_.flush_pending_ ? db_socket_readiness::read | db_socket_readiness::write
                                                                 : db_socket_readiness::read;
}

void postgresql_pipeline_io::exchange(state_type& state, std::size_t index, db_socket_readiness) {
    auto& slot = state.pool().slot(index);
    auto& data = slot.session_.pipeline_.io_;
    data.flush_pending_ = exchange_postgresql_io(*slot.connection_, data.flush_pending_);
}

// Flights abandoned before their results arrived leave results nobody reads;
// the connection must leave pipeline mode before the pool checks its session
// state for reuse.
bool postgresql_pipeline_io::reusable(state_type& state, std::size_t index) noexcept {
    auto& slot = state.pool().slot(index);
    auto* connection = slot.connection_;
    return slot.session_.pipeline_.pending_ == 0 && PQexitPipelineMode(connection) != 0;
}

template class db_pipeline_state<postgresql_pipeline_io>;

task<void> postgresql_driver::connect(pool_type& pool, std::size_t index, const operation_deadline& deadline_value) {
    const auto& config = pool.config();
    auto& slot = pool.slot(index);
    slot.session_.statements_.clear();
    auto tls_host = config.tls_.tls_host(config.host_);
    if (tls_host.ends_with('.') && !is_client_ip_address(tls_host)) {
        tls_host.remove_suffix(1);
    }
    // Pin the client encoding to UTF-8. Ruvia's strings are UTF-8 throughout,
    // and query parameters are sent in text format; without this the connection
    // inherits the server/database default encoding, so non-ASCII parameters and
    // result text would be misinterpreted on a non-UTF-8 database (e.g. LATIN1,
    // SQL_ASCII). libpq accepts client_encoding as a connection keyword.
    const bool use_tls = config.tls_.mode_ == client_tls_mode::verify_identity;
    const auto& identity = config.tls_.identity_;
    const char* const root_certificate =
        !use_tls ? "" : (config.tls_.ca_file_.has_value() ? tls_file_name_c_str(*config.tls_.ca_file_) : "system");
    {
        const auto resolved_hosts = make_postgresql_resolved_host_list(
            tls_host, co_await pool.resolve_host(index, deadline_value));
        const auto port = format_db_port(config.port_);
        const char* minimum_tls = config.tls_.minimum_version_ == tls_version::tls_1_3 ? "TLSv1.3" : config.tls_.minimum_version_ == tls_version::tls_1_2 ? "TLSv1.2"
                                                                                                                                                          : "TLSv1.1";
        const std::array<const char*, 14> keywords{"host", "hostaddr", "port", "user", "password", "dbname",
            "client_encoding", "sslmode", "sslrootcert", "sslcert", "sslkey", "gssencmode", "ssl_min_protocol_version",
            nullptr};
        const std::array<const char*, 14> values{resolved_hosts.hosts_.c_str(), resolved_hosts.addresses_.c_str(),
            port.data(), config.username_.c_str(), config.password_.c_str(), config.database_.c_str(), "UTF8",
            use_tls ? "verify-full" : "disable", root_certificate,
            identity.has_value() ? tls_file_name_c_str(identity->certificate_chain_file_) : "",
            identity.has_value() ? tls_file_name_c_str(identity->private_key_file_) : "", "disable", minimum_tls,
            nullptr};
        // PQconnectStartParams copies the parameter strings before returning.
        native_tls_context_scope tls_scope(use_tls ? tls_host : std::string_view{}, config.tls_.max_outgoing_records_);
        slot.connection_ = PQconnectStartParams(keywords.data(), values.data(), 0);
        tls_scope.rethrow_failure();
    }
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
        native_tls_context_scope tls_scope(use_tls ? tls_host : std::string_view{}, config.tls_.max_outgoing_records_);
        status = PQconnectPoll(slot.connection_);
        tls_scope.rethrow_failure();
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

bool postgresql_driver::reusable_connection(connection_type& connection) noexcept {
    return PQstatus(&connection) == CONNECTION_OK && PQtransactionStatus(&connection) == PQTRANS_IDLE;
}

// An idle session may legitimately receive asynchronous messages (NOTIFY,
// ParameterStatus, a notice), so pending bytes go to libpq first; a FATAL
// error ahead of a server-side close is read the same way. Whatever is still
// waiting behind them -- the end of the stream, or more unsolicited traffic --
// retires the connection.
bool postgresql_driver::idle_connection_alive(connection_type& connection) noexcept {
    const auto native = PQsocket(&connection);
    if (PQstatus(&connection) != CONNECTION_OK || native < 0) {
        return false;
    }
    const auto driver_socket = static_cast<db_slot_socket::native_socket_type>(native);
    auto status = peek_db_idle_socket(driver_socket);
    if (status == db_idle_socket_status::pending_data) {
        if (PQconsumeInput(&connection) == 0 || PQstatus(&connection) != CONNECTION_OK) {
            return false;
        }
        status = peek_db_idle_socket(driver_socket);
    }
    return status == db_idle_socket_status::quiet;
}

task<db_rows> postgresql_driver::query(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, const operation_deadline& deadline_value) {
    return run_cached_statement<db_rows>(pool, index, sql, params, deadline_value);
}

task<db_exec_result> postgresql_driver::execute(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, const operation_deadline& deadline_value) {
    return run_cached_statement<db_exec_result>(pool, index, sql, params, deadline_value);
}

// Retain the first libpq result so column metadata is available even for an
// empty row set, without consuming or copying the first row.
task<db_stream_start> postgresql_driver::start_stream(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, const operation_deadline& deadline_value) {
    if (send_stream_query(pool, index, sql, params)) {
        co_await flush_output(pool, index, deadline_value);
    }
    co_await wait_until_result_ready(pool, index, deadline_value);
    postgresql_result_owner first(PQgetResult(pool.slot(index).connection_));
    if (first.get() == nullptr) {
        throw postgresql_protocol_failure("PostgreSQL stream ended without its result");
    }
    db_stream_start result;
    const auto status = PQresultStatus(first.get());
    if (status == PGRES_SINGLE_TUPLE || status == PGRES_TUPLES_OK) {
        const auto count = static_cast<std::size_t>(PQnfields(first.get()));
        result.column_names_.reserve(count);
        populate_postgresql_column_names(result.column_names_, *first, count);
    }
    result.result_ = first.release();
    co_return result;
}

task<db_stream_step> postgresql_driver::read_stream_row(pool_type& pool, std::size_t index, void*& pending_result,
    std::vector<db_field>& fields, std::vector<std::string>& column_names, const operation_deadline& deadline_value) {
    auto* connection = pool.slot(index).connection_;
    postgresql_result_owner result_value(static_cast<PGresult*>(std::exchange(pending_result, nullptr)));
    if (result_value.get() == nullptr) {
        co_await wait_until_result_ready(pool, index, deadline_value);
        result_value.reset(PQgetResult(connection));
    }
    if (result_value.get() == nullptr) {
        throw postgresql_protocol_failure("PostgreSQL stream ended without its result");
    }
    const auto status = PQresultStatus(result_value.get());
    if (status == PGRES_SINGLE_TUPLE) {
        co_return materialize_stream_single_row(*result_value, fields, column_names);
    }
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK && status != PGRES_EMPTY_QUERY) {
        throw postgresql_error(
            *connection, "PostgreSQL stream", db_error::code_type::statement_failed, result_value.get());
    }
    result_value.reset();
    co_await wait_until_result_ready(pool, index, deadline_value);
    result_value.reset(PQgetResult(connection));
    if (result_value.get() != nullptr) {
        throw postgresql_protocol_failure("PostgreSQL stream command boundary is missing");
    }
    co_await wait_until_result_ready(pool, index, deadline_value);
    result_value.reset(PQgetResult(connection));
    if (result_value.get() == nullptr || PQresultStatus(result_value.get()) != PGRES_PIPELINE_SYNC) {
        throw postgresql_protocol_failure("PostgreSQL stream ended without its Sync");
    }
    if (PQexitPipelineMode(connection) == 0) {
        throw postgresql_error(*connection, "PQexitPipelineMode", db_error::code_type::protocol_error);
    }
    co_return status == PGRES_TUPLES_OK ? db_stream_end::exhausted : db_stream_end::no_row_set;
}

// Abandoning a libpq single-row result still requires draining the whole
// command. Closing the connection instead is bounded and keeps the worker
// non-blocking.
task<void> postgresql_driver::close_stream(pool_type& pool, std::size_t index, void*& pending_result, const operation_deadline&) {
    discard_stream(std::exchange(pending_result, nullptr));
    pool.close_slot(index);
    co_return;
}

void postgresql_driver::discard_stream(void* pending_result) noexcept {
    PQclear(static_cast<PGresult*>(pending_result));
}

}  // namespace ruvia::detail
