#include <mysql.h>

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/core/error_text.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_mysql_runtime.h"
#include "db/db_pool.h"
#include "db/db_sql.h"

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

// A db_config deadline is this pool's to enforce, not the driver's. Reporting
// its expiry to MariaDB as MYSQL_WAIT_TIMEOUT only works where libmariadb has
// a matching timeout option of its own -- it has one for connect, none for a
// statement -- so a query deadline would be handed over and dropped. Failing
// here instead ends the operation, and the caller closes the connection: the
// statement may still be running server-side, which is exactly what a
// client-side timeout means.
task<int> wait_for_mysql(mariadb_pool& pool, std::size_t index, int status, const operation_timeout& deadline_value) {
    pool.leases().throw_if_aborted(index);
    auto& slot = pool.slot(index);
    auto& connection = *slot.connection_;
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
        if (pool.leases().arm_deadline(index, selected_deadline.timeout_, db_deadline_kind::sleep)) {
            struct deadline_awaiter final {
                mariadb_pool& pool_;
                std::size_t index_;

                [[nodiscard]] bool await_ready() noexcept {
                    return pool_.leases().poll_deadline(index_);
                }
                void await_suspend(std::coroutine_handle<> handle) noexcept {
                    pool_.slot(index_).deadline_continuation_ = handle;
                }
                void await_resume() const noexcept {}
            };
            db_slot_active_wait_guard active_wait(slot);
            co_await deadline_awaiter{pool, index};
        }
        (void)pool.leases().clear_deadline(index);
        slot.deadline_continuation_ = {};
        pool.leases().throw_if_aborted(index);
        if (slot.close_requested_) {
            throw pool.closing_error();
        }
        if (deadline_value.expired() || selected_deadline.source_ == mysql_wait_deadline_source::operation) {
            throw pool.timed_out();
        }
        co_return MYSQL_WAIT_TIMEOUT;
    }

    const auto native = mysql_native_socket(connection);
    if (native == db_slot_socket::invalid_socket) {
        throw mysql_socket_error("MariaDB returned an invalid socket for an active wait",
            std::make_error_code(std::errc::bad_file_descriptor));
    }
    const auto wait_result = co_await pool.wait_socket(index, native, wanted, selected_deadline.timeout_, deadline_value);
    if (wait_result.expired_) {
        if (selected_deadline.source_ == mysql_wait_deadline_source::operation) {
            throw pool.timed_out();
        }
        co_return MYSQL_WAIT_TIMEOUT;
    }
    if (wait_result.error_) {
        throw mysql_socket_error("waiting for MariaDB socket", wait_result.error_);
    }
    switch (wait_result.ready_) {
        case db_socket_readiness::read:
            co_return MYSQL_WAIT_READ;
        case db_socket_readiness::write:
            co_return MYSQL_WAIT_WRITE;
        case db_socket_readiness::error:
            co_return MYSQL_WAIT_EXCEPT;
        default:
            co_return MYSQL_WAIT_TIMEOUT;
    }
}

task<void> run_mysql_statement(mariadb_pool& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    auto& connection = *pool.slot(index).connection_;
    std::pmr::string interpolated_sql(pmr_resource_or_default(resource));
    if (!params.empty()) {
        interpolated_sql = interpolate_sql(connection, sql, params, resource);
        sql = interpolated_sql;
    }
    validate_mariadb_sql_length(sql.size());
    int query_result = 0;
    int status = mysql_real_query_start(&query_result, &connection, sql.data(), static_cast<unsigned long>(sql.size()));
    while (status != 0) {
        status = mysql_real_query_cont(
            &query_result, &connection, co_await wait_for_mysql(pool, index, status, deadline_value));
    }
    if (query_result != 0) {
        throw mysql_error(connection, "mysql_real_query", db_error::code_type::statement_failed);
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

// A CALL that returns rows ends with a separate status result, and the
// connection refuses every later command until that result has been read.
// Only status results may follow; a further result set cannot be returned.
task<void> finish_mysql_results(mariadb_pool& pool, std::size_t index, const operation_timeout& deadline_value) {
    auto& connection = *pool.slot(index).connection_;
    while (mysql_more_results(&connection) != 0) {
        int next_result = 0;
        int status = mysql_next_result_start(&next_result, &connection);
        while (status != 0) {
            status = mysql_next_result_cont(
                &next_result, &connection, co_await wait_for_mysql(pool, index, status, deadline_value));
        }
        if (next_result != 0) {
            throw mysql_error(connection, "mysql_next_result", db_error::code_type::statement_failed);
        }
        if (mysql_field_count(&connection) != 0) {
            throw std::invalid_argument("SQL must not return more than one result set");
        }
    }
}

[[nodiscard]] operation_timeout mysql_stream_deadline(const mariadb_pool& pool, const operation_options& options) {
    return operation_timeout(options.timeout_).constrained_by(pool.config().query_timeout_);
}

}  // namespace

task<void> mariadb_driver::connect(pool_type& pool, std::size_t index, const operation_timeout& deadline_value) {
    const auto& config = pool.config();
    auto& slot = pool.slot(index);
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
    // Pin the connection charset before connecting. mysql_real_escape_string --
    // the interpolate_sql injection defense -- escapes according to the
    // connection charset, and is bypassable on multibyte charsets where a valid
    // character can end in 0x5C (GBK/Big5/SJIS, the classic backslash-swallowing
    // injection). utf8mb4 keeps 0x5C meaning only backslash, so escaping stays
    // safe regardless of the server or client library default, and it carries
    // full Unicode. Set here (not via SET NAMES) so it also governs the handshake.
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
}

void mariadb_driver::close_connection(connection_type& connection) noexcept {
    mysql_close(&connection);
}

task<db_rows> mariadb_driver::query(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    co_await run_mysql_statement(pool, index, sql, params, resource, deadline_value);
    auto& connection = *pool.slot(index).connection_;

    auto result_value = db_result_access::make_result(resource);
    auto* raw_result = co_await store_mysql_result(pool, index, deadline_value);
    if (raw_result == nullptr) {
        // A lost connection also yields no result, and libmariadb resets the
        // field count when it drops the connection, so only mysql_errno()
        // tells a failed transfer from a statement without a row set.
        if (mysql_field_count(&connection) != 0 || mysql_errno(&connection) != 0) {
            throw mysql_error(connection, "mysql_store_result", db_error::code_type::statement_failed);
        }
        throw std::invalid_argument("query() requires row-producing SQL");
    }

    db_result_access::own_raw_result(result_value, raw_result, &free_stored_result);
    co_await finish_mysql_results(pool, index, deadline_value);
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
        result_column_names.emplace_back(fields_value[i].name, static_cast<std::size_t>(fields_value[i].name_length));
    }
    while (auto* row = mysql_fetch_row(raw_result)) {
        const auto* lengths = mysql_fetch_lengths(raw_result);
        const auto row_start = result_fields.size();
        for (std::size_t i = 0; i < field_count; ++i) {
            if (row[i] == nullptr) {
                result_fields.push_back(db_result_access::null_field(resource));
                continue;
            }
            result_fields.push_back(db_result_access::borrowed_field(std::string_view(row[i], lengths[i]), resource));
        }
        result_rows.push_back(db_result_access::borrowed_row(result_fields.data() + row_start, field_count,
            result_column_names.data(), result_column_names.size(), resource));
    }

    co_return result_value;
}

task<db_exec_result> mariadb_driver::execute(pool_type& pool, std::size_t index, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline_value) {
    co_await run_mysql_statement(pool, index, sql, params, resource, deadline_value);
    auto& connection = *pool.slot(index).connection_;
    const auto affected_rows = static_cast<std::uint64_t>(mysql_affected_rows(&connection));
    const auto insert_id = static_cast<std::uint64_t>(mysql_insert_id(&connection));
    auto* raw_result = co_await store_mysql_result(pool, index, deadline_value);
    if (raw_result != nullptr) {
        free_stored_result(raw_result);
        throw std::invalid_argument("execute() does not accept row-producing SQL");
    }
    if (mysql_field_count(&connection) != 0 || mysql_errno(&connection) != 0) {
        throw mysql_error(connection, "mysql_store_result", db_error::code_type::statement_failed);
    }
    co_return db_result_access::make_exec_result(
        affected_rows, insert_id == 0 ? std::nullopt : std::optional<std::uint64_t>(insert_id));
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
        co_return db_stream_start{.result_ = nullptr, .returns_rows_ = false};
    }
    co_return db_stream_start{.result_ = raw_result, .returns_rows_ = true};
}

task<std::optional<db_row>> mariadb_driver::read_stream_row(pool_type& pool, std::size_t index, void* result_value,
    std::pmr::memory_resource* resource, const operation_options& options) {
    if (result_value == nullptr) {
        co_return std::nullopt;
    }

    typename pool_type::leases_type::cancellation_guard cancellation(pool.leases(), index, options.stop_token_);
    auto* raw_result = static_cast<MYSQL_RES*>(result_value);
    try {
        pool.leases().throw_if_aborted(index);
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
                output_fields.push_back(db_result_access::owned_field(std::string_view(row[i], lengths[i]), resource));
            }
            co_return output_row;
        }
    } catch (...) {
        discard_unbuffered_result(raw_result);
        pool.close_slot(index);
        cancellation.finish();
        pool.leases().release(index);
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

    typename pool_type::leases_type::cancellation_guard cancellation(pool.leases(), index, options.stop_token_);
    auto* raw_result = static_cast<MYSQL_RES*>(result_value);
    try {
        pool.leases().throw_if_aborted(index);
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
        co_await finish_mysql_results(pool, index, deadline_value);
        cancellation.finish();
        pool.leases().release(index);
    } catch (...) {
        discard_unbuffered_result(raw_result);
        pool.close_slot(index);
        cancellation.finish();
        pool.leases().release(index);
        throw;
    }
}

void mariadb_driver::discard_stream(void* result) noexcept {
    discard_unbuffered_result(result);
}

}  // namespace ruvia::detail
