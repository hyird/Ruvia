#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/core/failure_report.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_pool.h"
#include "db/db_postgresql.h"

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

// One socket readiness wait. db_config's read and write timeouts bound one
// socket wait in that direction; a flush waits to write, so it is bounded by
// the write timeout. Expiry fails the operation like its overall deadline
// does: libpq has no per-I/O timeout to hand the wait back to.
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
        if (wait_result.error_) {
            throw db_error(db_error::code_type::io_error, db_driver::postgresql,
                std::string("PostgreSQL socket wait failed: ").append(error_code_text(wait_result.error_)), wait_result.error_.value());
        }
    }
};

[[nodiscard]] postgresql_socket_wait wait_for_postgresql(postgresql_pool& pool, std::size_t index,
    db_socket_readiness wanted, const operation_timeout& deadline_value) {
    const auto remaining = deadline_value.remaining();
    if (remaining.has_value() && remaining->count() <= 0) {
        throw pool.timed_out();
    }
    const auto& config = pool.config();
    const auto io_timeout = has_db_socket_readiness(wanted, db_socket_readiness::write) ? config.write_timeout_
                                                                                        : config.read_timeout_;
    const auto native = PQsocket(pool.slot(index).connection_);
    if (native < 0) {
        throw db_error(
            db_error::code_type::io_error, db_driver::postgresql, "PostgreSQL connection socket is unavailable");
    }
    return postgresql_socket_wait{pool,
        pool.wait_socket(index, static_cast<db_slot_socket::native_socket_type>(native), wanted,
            earlier_timeout(remaining, io_timeout), deadline_value)};
}

// Called only after PQflush() reports pending output. Wait for read OR write
// readiness and drain input before retrying the flush. A server blocked sending
// results/notices would otherwise never read our query and both sides deadlock
// on full socket buffers.
task<void> flush_output(postgresql_pool& pool, std::size_t index, const operation_timeout& deadline_value) {
    auto* connection = pool.slot(index).connection_;
    while (true) {
        co_await wait_for_postgresql(
            pool, index, db_socket_readiness::read | db_socket_readiness::write, deadline_value);
        // PQconsumeInput() never blocks; with no input available it is a
        // no-op, so calling it after a write-readiness wake is harmless.
        if (PQconsumeInput(connection) == 0) {
            throw postgresql_error(*connection, "PQconsumeInput", db_error::code_type::io_error);
        }
        const auto status = PQflush(connection);
        if (status == 0) {
            co_return;
        }
        if (status < 0) {
            throw postgresql_error(*connection, "PQflush", db_error::code_type::io_error);
        }
    }
}

task<void> wait_until_result_ready(postgresql_pool& pool, std::size_t index, const operation_timeout& deadline_value) {
    auto* connection = pool.slot(index).connection_;
    while (PQisBusy(connection) != 0) {
        co_await wait_for_postgresql(pool, index, db_socket_readiness::read, deadline_value);
        if (PQconsumeInput(connection) == 0) {
            throw postgresql_error(*connection, "PQconsumeInput", db_error::code_type::io_error);
        }
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

[[nodiscard]] bool flush_postgresql(pg_conn& connection) {
    const auto status = PQflush(&connection);
    if (status < 0) {
        throw postgresql_error(connection, "PQflush", db_error::code_type::io_error);
    }
    return status != 0;
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

// The commands of one buffered statement, sent in pipeline mode as a single
// round trip. A first use runs as the unnamed statement. Otherwise a Close of
// a statement evicted to make room and a Close and Parse of a statement new
// to the connection precede the statement. A Sync ends the flight. The Close
// ahead of Parse lets a name the server may still hold (a statement forgotten
// as stale) parse again.
struct postgresql_flight final {
    // The cache entry of a named statement.
    std::optional<std::size_t> statement_;
    // Commands queued ahead of the statement.
    std::size_t setup_commands_;
    bool parses_;
    bool flush_pending_;
};

[[nodiscard]] postgresql_flight send_cached_statement(
    postgresql_pool& pool, std::size_t index, std::string_view sql, std::span<const db_value> params) {
    validate_postgresql_statement(sql, params);
    auto& slot = pool.slot(index);
    auto* connection = slot.connection_;
    auto encoded = encode_postgresql_params(params, pool.resource());
    const auto lookup = slot.statements_.find_or_insert(sql, encoded.types_);
    const auto count = static_cast<int>(params.size());
    const auto* values = encoded.values_.empty() ? nullptr : encoded.values_.data();
    const auto* types = encoded.types_.empty() ? nullptr : encoded.types_.data();
    // Each failure below leaves the connection outside its idle protocol
    // state, so it is an I/O failure and the pool closes the slot, which
    // also discards the cache entries of this connection.
    const auto queue_failure = [connection](std::string_view operation) {
        return postgresql_error(*connection, operation, db_error::code_type::io_error);
    };
    if (PQenterPipelineMode(connection) == 0) {
        throw queue_failure("PQenterPipelineMode");
    }
    postgresql_flight flight{lookup.index_, 0, lookup.inserted_, false};
    if (!lookup.index_.has_value()) {
        if (PQsendQueryParams(connection, sql.data(), count, types, values, nullptr, nullptr, 0) == 0) {
            throw queue_failure("PQsendQueryParams");
        }
    } else {
        const auto* name = slot.statements_.name(*lookup.index_);
        if (lookup.evicted_.has_value()) {
            if (PQsendClosePrepared(connection, lookup.evicted_->data()) == 0) {
                throw queue_failure("PQsendClosePrepared");
            }
            ++flight.setup_commands_;
        }
        if (lookup.inserted_) {
            if (PQsendClosePrepared(connection, name) == 0) {
                throw queue_failure("PQsendClosePrepared");
            }
            if (PQsendPrepare(connection, name, sql.data(), count, types) == 0) {
                throw queue_failure("PQsendPrepare");
            }
            flight.setup_commands_ += 2;
        }
        if (PQsendQueryPrepared(connection, name, count, values, nullptr, nullptr, 0) == 0) {
            throw queue_failure("PQsendQueryPrepared");
        }
    }
    if (PQsendPipelineSync(connection) == 0) {
        throw queue_failure("PQsendPipelineSync");
    }
    flight.flush_pending_ = flush_postgresql(*connection);
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

// Runs one buffered statement as a cached named statement and reads its whole
// flight, up to the Sync, so the connection is idle again whenever the
// statement itself failed. A failure that leaves no such boundary (transport
// loss, COPY, an unexpected result) is thrown at once, and the pool closes the
// slot. A stale cached statement that failed before running, outside a
// transaction block, is prepared again and run once more.
template <typename result_type>
task<result_type> run_cached_statement(postgresql_pool& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    constexpr bool returns_rows = std::is_same_v<result_type, db_rows>;
    constexpr std::string_view operation = returns_rows ? "PostgreSQL query" : "PostgreSQL execute";
    auto* connection = pool.slot(index).connection_;
    auto& statements = pool.slot(index).statements_;
    for (bool may_retry = true;; may_retry = false) {
        const auto flight = send_cached_statement(pool, index, sql, params);
        if (flight.flush_pending_) {
            co_await flush_output(pool, index, deadline_value);
        }
        auto output = db_result_access::make_result(resource);
        bool returned_rows = false;
        std::uint64_t affected_rows = 0;
        postgresql_result_owner failure(nullptr);
        bool statement_failed = false;
        bool parsed = false;
        // Index of the command whose results are being read; each command's
        // results end with a null result, and the Sync result follows the last.
        std::size_t command = 0;
        while (true) {
            // Keep readiness in this frame, including the final protocol boundary.
            while (PQisBusy(connection) != 0) {
                co_await wait_for_postgresql(pool, index, db_socket_readiness::read, deadline_value);
                if (PQconsumeInput(connection) == 0) {
                    throw postgresql_error(*connection, "PQconsumeInput", db_error::code_type::io_error);
                }
            }
            postgresql_result_owner result_value(PQgetResult(connection));
            if (result_value.get() == nullptr) {
                if (++command > flight.setup_commands_ + 1) {
                    throw postgresql_protocol_failure("PostgreSQL pipeline ended without its Sync");
                }
                continue;
            }
            const auto status = PQresultStatus(result_value.get());
            if (status == PGRES_PIPELINE_SYNC) {
                if (command != flight.setup_commands_ + 1) {
                    throw postgresql_protocol_failure("PostgreSQL pipeline Sync arrived early");
                }
                break;
            }
            if (status == PGRES_PIPELINE_ABORTED) {
                continue;
            }
            if (status == PGRES_FATAL_ERROR) {
                if (PQstatus(connection) == CONNECTION_BAD) {
                    throw postgresql_error(*connection, operation, db_error::code_type::io_error, result_value.get());
                }
                if (failure.get() == nullptr) {
                    statement_failed = command == flight.setup_commands_;
                    failure.reset(result_value.release());
                }
                continue;
            }
            if (command < flight.setup_commands_) {
                if (status != PGRES_COMMAND_OK) {
                    throw postgresql_protocol_failure("PostgreSQL statement setup returned an unexpected result");
                }
                parsed = parsed || (flight.parses_ && command + 1 == flight.setup_commands_);
                continue;
            }
            if (command != flight.setup_commands_ ||
                (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK && status != PGRES_EMPTY_QUERY)) {
                throw postgresql_protocol_failure("PostgreSQL statement returned an unsupported result status");
            }
            if (status != PGRES_TUPLES_OK) {
                affected_rows = postgresql_affected_rows(*result_value);
                continue;
            }
            if (returned_rows) {
                throw postgresql_protocol_failure("PostgreSQL returned multiple tuple results");
            }
            returned_rows = true;
            if constexpr (returns_rows) {
                materialize_borrowed_result(output, *result_value, resource);
                db_result_access::own_raw_result(output, result_value.release(), &free_postgresql_result);
            }
        }
        if (PQexitPipelineMode(connection) == 0) {
            throw postgresql_error(*connection, "PQexitPipelineMode", db_error::code_type::protocol_error);
        }
        if (flight.parses_ && !parsed) {
            statements.forget(*flight.statement_);
        }
        if (failure.get() != nullptr) {
            if (statement_failed && flight.statement_.has_value() && !flight.parses_ &&
                postgresql_stale_statement(*failure)) {
                statements.forget(*flight.statement_);
                if (may_retry && postgresql_stale_before_execution(*failure) &&
                    PQtransactionStatus(connection) == PQTRANS_IDLE) {
                    continue;
                }
            }
            throw postgresql_error(*connection, operation, db_error::code_type::statement_failed, failure.get());
        }
        if constexpr (returns_rows) {
            if (!returned_rows) {
                throw std::invalid_argument("query() requires row-producing SQL");
            }
            co_return output;
        } else {
            // The whole command is read before a row set is refused, leaving
            // the connection idle.
            if (returned_rows) {
                throw std::invalid_argument("execute() does not accept row-producing SQL");
            }
            co_return db_result_access::make_exec_result(affected_rows);
        }
    }
}

[[nodiscard]] operation_timeout postgresql_stream_deadline(
    const postgresql_pool& pool, const operation_options& options) {
    return operation_timeout(options.timeout_).constrained_by(pool.config().query_timeout_);
}

}  // namespace

task<void> postgresql_driver::connect(pool_type& pool, std::size_t index, const operation_timeout& deadline_value) {
    const auto& config = pool.config();
    auto& slot = pool.slot(index);
    slot.statements_.clear();
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
    typename pool_type::leases_type::cancellation_guard cancellation(pool.leases(), index, options.stop_token_);
    bool slot_released = false;
    try {
        pool.leases().throw_if_aborted(index);
        const auto deadline_value = postgresql_stream_deadline(pool, options);
        co_await wait_until_result_ready(pool, index, deadline_value);
        postgresql_result_owner result_value(PQgetResult(connection));
        if (result_value.get() == nullptr) {
            slot_released = true;
            cancellation.finish();
            pool.leases().release(index);
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
            pool.leases().release(index);
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
            pool.leases().release(index);
        }
        throw;
    }
}

task<void> postgresql_driver::close_stream(
    pool_type& pool, std::size_t index, void*, std::pmr::memory_resource*, const operation_options& options) {
    typename pool_type::leases_type::cancellation_guard cancellation(pool.leases(), index, options.stop_token_);
    // Abandoning a libpq single-row result still requires draining the whole
    // command. Closing is bounded and keeps the worker non-blocking.
    pool.close_slot(index);
    cancellation.finish();
    pool.leases().release(index);
    if (options.stop_token_.stop_requested()) {
        throw db_error(db_error::code_type::cancelled, db_driver::postgresql, "database operation cancelled");
    }
    co_return;
}

void postgresql_driver::discard_stream(void*) noexcept {}

}  // namespace ruvia::detail
