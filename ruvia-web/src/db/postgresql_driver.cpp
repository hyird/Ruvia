#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/error_text.h"
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
[[noreturn]] void throw_postgresql_statement_error(
    PGconn& connection, std::string_view operation, postgresql_result_owner& result_value) {
    auto error = postgresql_error(connection, operation, db_error::code_type::statement_failed, result_value.get());
    result_value.reset();
    throw error;
}

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
            fields_value.data() + row_start, field_count, column_names.data(), column_names.size(), resource));
    }
}

[[nodiscard]] db_row materialize_owned_single_row(PGresult& result_value, std::pmr::memory_resource* resource) {
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

// db_config's read and write timeouts bound one socket wait in that direction;
// a flush waits to write, so it is bounded by the write timeout. Expiry fails
// the operation like its overall deadline does: libpq has no per-I/O timeout
// to hand the wait back to.
task<void> wait_for_postgresql(postgresql_pool& pool, std::size_t index, db_socket_readiness wanted,
    const operation_timeout& deadline_value) {
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
    const auto wait_result = co_await pool.wait_socket(index, static_cast<db_slot_socket::native_socket_type>(native),
        wanted, earlier_timeout(remaining, io_timeout), deadline_value);
    if (wait_result.expired_) {
        throw pool.timed_out();
    }
    if (wait_result.error_) {
        throw db_error(db_error::code_type::io_error, db_driver::postgresql,
            std::string("PostgreSQL socket wait failed: ").append(error_code_text(wait_result.error_)), wait_result.error_.value());
    }
}

// libpq's non-blocking send protocol: while PQflush() reports pending output,
// wait for read OR write readiness and drain input when readable. A server
// blocked sending results/notices would otherwise never read our query and
// both sides deadlock on full socket buffers.
task<void> flush_output(postgresql_pool& pool, std::size_t index, const operation_timeout& deadline_value) {
    auto* connection = pool.slot(index).connection_;
    while (true) {
        const auto status = PQflush(connection);
        if (status == 0) {
            co_return;
        }
        if (status < 0) {
            throw postgresql_error(*connection, "PQflush", db_error::code_type::io_error);
        }
        co_await wait_for_postgresql(
            pool, index, db_socket_readiness::read | db_socket_readiness::write, deadline_value);
        // PQconsumeInput() never blocks; with no input available it is a
        // no-op, so calling it after a write-readiness wake is harmless.
        if (PQconsumeInput(connection) == 0) {
            throw postgresql_error(*connection, "PQconsumeInput", db_error::code_type::io_error);
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

// `sql` is NUL-terminated (db_pool's hook contract), so it reaches libpq
// without a copy.
task<void> send_query(postgresql_pool& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, const operation_timeout& deadline_value, bool single_row) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if (sql.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SQL must not contain NUL bytes");
    }
    if (!std::in_range<int>(params.size())) {
        throw std::invalid_argument("too many PostgreSQL query parameters");
    }
    auto* connection = pool.slot(index).connection_;
    auto encoded = encode_postgresql_params(params, pool.resource());
    const auto* values = encoded.values_.empty() ? nullptr : encoded.values_.data();
    const auto* lengths = encoded.lengths_.empty() ? nullptr : encoded.lengths_.data();
    if (PQsendQueryParams(
            connection, sql.data(), static_cast<int>(params.size()), nullptr, values, lengths, nullptr, 0) == 0) {
        throw postgresql_error(*connection, "PQsendQueryParams", db_error::code_type::statement_failed);
    }
    if (single_row && PQsetSingleRowMode(connection) == 0) {
        throw postgresql_error(*connection, "PQsetSingleRowMode", db_error::code_type::statement_failed);
    }
    co_await flush_output(pool, index, deadline_value);
}

[[nodiscard]] operation_timeout postgresql_stream_deadline(
    const postgresql_pool& pool, const operation_options& options) {
    return operation_timeout(options.timeout_).constrained_by(pool.config().query_timeout_);
}

}  // namespace

task<void> postgresql_driver::connect(pool_type& pool, std::size_t index, const operation_timeout& deadline_value) {
    const auto& config = pool.config();
    auto& slot = pool.slot(index);
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
    co_await send_query(pool, index, sql, params, deadline_value, false);
    auto* connection = pool.slot(index).connection_;

    auto output = db_result_access::make_result(resource);
    bool retained_tuple_result = false;
    while (true) {
        co_await wait_until_result_ready(pool, index, deadline_value);
        postgresql_result_owner result_value(PQgetResult(connection));
        if (result_value.get() == nullptr) {
            break;
        }
        const auto status = PQresultStatus(result_value.get());
        if (!successful_result_status(status)) {
            throw_postgresql_statement_error(*connection, "PostgreSQL query", result_value);
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

task<db_exec_result> postgresql_driver::execute(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource*, const operation_timeout& deadline_value) {
    co_await send_query(pool, index, sql, params, deadline_value, false);
    auto* connection = pool.slot(index).connection_;

    std::uint64_t affected_rows = 0;
    while (true) {
        co_await wait_until_result_ready(pool, index, deadline_value);
        postgresql_result_owner result_value(PQgetResult(connection));
        if (result_value.get() == nullptr) {
            break;
        }
        const auto status = PQresultStatus(result_value.get());
        if (!successful_result_status(status)) {
            throw_postgresql_statement_error(*connection, "PostgreSQL execute", result_value);
        }
        if (status == PGRES_TUPLES_OK) {
            result_value.reset();
            throw std::invalid_argument("execute() does not accept row-producing SQL");
        }
        affected_rows = postgresql_affected_rows(*result_value);
    }
    co_return db_result_access::make_exec_result(affected_rows);
}

// Single-row mode reports whether the statement produced rows only with its
// first result, so the stream starts optimistically and read_stream_row()
// refuses a statement without a row set.
task<db_stream_start> postgresql_driver::start_stream(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource*, const operation_timeout& deadline_value) {
    co_await send_query(pool, index, sql, params, deadline_value, true);
    co_return db_stream_start{};
}

task<std::optional<db_row>> postgresql_driver::read_stream_row(pool_type& pool, std::size_t index, void*,
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
            co_return materialize_owned_single_row(*result_value, resource);
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
