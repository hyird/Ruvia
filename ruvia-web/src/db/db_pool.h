#pragma once

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_transaction.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_backend.h"

#include "db/db_config_storage.h"
#include "db/db_host_resolution.h"
#include "db/db_slot_socket.h"
#include "integration/slot_pool.h"

struct st_mysql;
struct pg_conn;

// One worker-local database pool, shared by every driver. The pool owns lease
// scheduling, cancellation, deadlines, connection lifecycle, and the statement,
// stream and transaction shapes; a driver supplies only its connection type and
// the protocol hooks that talk to its client library.

namespace ruvia::detail {

// A database alias opens one connection per worker: every operation on one
// worker is serialized behind it, and the worker count is the pool size.
inline constexpr std::size_t db_connections_per_worker = 1;

enum class db_deadline_kind : std::uint8_t {
    resolve,
    socket,
    // A driver-requested timed pause without a socket wait (MariaDB connect).
    sleep,
};

// What a driver hook reports after starting an unbuffered statement.
struct db_stream_start final {
    // Driver-owned result handle handed to the stream (may be null).
    void* result_{nullptr};
    // False when the statement produced no row set; the slot is still clean.
    bool returns_rows_{true};
};

template <typename driver_type>
class db_pool;

// The configured port as a NUL-terminated buffer, the form both asio's
// resolver and libpq take it in.
[[nodiscard]] std::array<char, 6> format_db_port(std::uint16_t port);

// The driver hooks. Statement hooks run on a connected, leased slot under a
// deadline already constrained by the query timeout. `sql` views are
// NUL-terminated. Hooks throw on failure; the pool closes the slot.
struct mariadb_driver final {
    using connection_type = st_mysql;
    using pool_type = db_pool<mariadb_driver>;
    static constexpr db_driver driver = db_driver::mariadb;
    static constexpr std::string_view name{"MariaDB"};

    static task<void> connect(pool_type& pool, std::size_t slot, const operation_timeout& deadline);
    static void close_connection(connection_type& connection) noexcept;
    static task<db_rows> query(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline);
    static task<db_exec_result> execute(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline);
    static task<db_stream_start> start_stream(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline);
    static task<std::optional<db_row>> read_stream_row(pool_type& pool, std::size_t slot, void* result,
        std::pmr::memory_resource* resource, const operation_options& options);
    static task<void> close_stream(pool_type& pool, std::size_t slot, void* result,
        std::pmr::memory_resource* resource, const operation_options& options);
    static void discard_stream(void* result) noexcept;
};

struct postgresql_driver final {
    using connection_type = pg_conn;
    using pool_type = db_pool<postgresql_driver>;
    static constexpr db_driver driver = db_driver::postgresql;
    static constexpr std::string_view name{"PostgreSQL"};

    static task<void> connect(pool_type& pool, std::size_t slot, const operation_timeout& deadline);
    static void close_connection(connection_type& connection) noexcept;
    static task<db_rows> query(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline);
    static task<db_exec_result> execute(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline);
    static task<db_stream_start> start_stream(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline);
    static task<std::optional<db_row>> read_stream_row(pool_type& pool, std::size_t slot, void* result,
        std::pmr::memory_resource* resource, const operation_options& options);
    static task<void> close_stream(pool_type& pool, std::size_t slot, void* result,
        std::pmr::memory_resource* resource, const operation_options& options);
    static void discard_stream(void* result) noexcept;
};

// One pooled connection. The driver owns the native socket; db_slot_socket
// waits on a Ruvia-owned duplicate of it.
template <typename driver_type>
struct db_slot final {
    explicit db_slot(asio::io_context& io_context)
        : resolver_(io_context),
          wait_socket_(io_context) {}

    ~db_slot() {
        if (wait_active_) {
            std::terminate();
        }
    }

    db_slot(db_slot&&) noexcept = default;
    db_slot& operator=(db_slot&&) noexcept = default;

    asio::ip::tcp::resolver resolver_;
    typename driver_type::connection_type* connection_{nullptr};
    db_slot_socket wait_socket_;
    // A sleep wait parks here until its deadline or a close resumes it.
    std::coroutine_handle<> deadline_continuation_{};
    bool connected_{false};
    // Shutdown may request closure while an asynchronous wait still borrows
    // this slot and its socket. The transport stays alive until that wait
    // resumes and the driving coroutine performs the final close; destroying
    // it immediately would leave the queued Asio handler dangling.
    bool wait_active_{false};
    bool close_requested_{false};
};

// Marks a slot as borrowed by a suspended wait for the wait's duration.
template <typename driver_type>
class db_slot_active_wait_guard final {
public:
    explicit db_slot_active_wait_guard(db_slot<driver_type>& slot) noexcept
        : slot_(slot) {
        if (slot_.wait_active_) {
            std::terminate();
        }
        slot_.wait_active_ = true;
    }
    db_slot_active_wait_guard(const db_slot_active_wait_guard&) = delete;
    db_slot_active_wait_guard& operator=(const db_slot_active_wait_guard&) = delete;
    ~db_slot_active_wait_guard() {
        slot_.wait_active_ = false;
    }

private:
    db_slot<driver_type>& slot_;
};

template <typename driver_type>
class db_pool final {
public:
    using slot_type = db_slot<driver_type>;

    struct slot_policy final {
        using owner_type = db_pool;
        using deadline_kind_type = db_deadline_kind;
        [[noreturn]] static void throw_acquire_failure(const db_pool& pool, const pool_waiter_result& result);
        [[noreturn]] static void throw_aborted(const db_pool& pool, pool_slot_abort_reason reason);
        static void expire_slot(db_pool& pool, std::size_t slot, db_deadline_kind kind) noexcept;
        static void cancel_slot(db_pool& pool, std::size_t slot) noexcept;
    };
    using leases_type = slot_pool<slot_policy>;

    db_pool(asio::io_context& io_context, const worker_handle& worker_value, db_config_storage config,
        std::pmr::memory_resource* resource);
    db_pool(asio::io_context&, worker_handle&&, db_config_storage, std::pmr::memory_resource*) = delete;
    ~db_pool();

    db_pool(const db_pool&) = delete;
    db_pool& operator=(const db_pool&) = delete;

    [[nodiscard]] task<void> connect();
    void close_now() noexcept;

    // The only execution path for raw SQL, compiled queries and transactions.
    task<db_rows> query(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options);
    task<db_exec_result> execute(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options);
    task<db_stream_result> stream(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options);
    task<std::optional<db_row>> read_stream_row(std::size_t slot, void* result,
        std::pmr::memory_resource* resource, const operation_options& options);
    task<void> close_stream(std::size_t slot, void* result, std::pmr::memory_resource* resource,
        const operation_options& options);
    void abort_stream(std::size_t slot, void* result) noexcept;
    task<db_rows> query_on_transaction_slot(std::size_t slot, std::pmr::string sql,
        std::pmr::vector<db_value> params, std::pmr::memory_resource* resource, const operation_options& options);
    task<db_exec_result> execute_on_transaction_slot(std::size_t slot, std::pmr::string sql,
        std::pmr::vector<db_value> params, std::pmr::memory_resource* resource, const operation_options& options);
    task<db_transaction> begin_transaction(std::pmr::memory_resource* resource,
        operation_options operation_options_value, db_transaction_options transaction_options);
    task<void> commit_transaction(
        std::size_t slot, std::pmr::memory_resource* resource, const operation_options& options);
    task<void> rollback_transaction(
        std::size_t slot, std::pmr::memory_resource* resource, const operation_options& options);
    void abort_transaction(std::size_t slot) noexcept;

    // Services for driver hooks.
    [[nodiscard]] const db_config_storage& config() const noexcept {
        return config_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }
    [[nodiscard]] slot_type& slot(std::size_t index) noexcept {
        return slots_[index];
    }
    [[nodiscard]] leases_type& leases() noexcept {
        return leases_;
    }
    // Closes the slot's connection now, or -- while a wait still borrows it --
    // wakes that wait so the driving coroutine finishes the close.
    void close_slot(std::size_t index) noexcept;
    task<db_resolved_addresses_type> resolve_host(std::size_t index, const operation_timeout& deadline);
    // One readiness wait on the driver's current socket under an optional
    // per-wait timeout. Cancellation, closing, operation timeout, and wait
    // failures are reported as db_error; slot-deadline expiry and socket
    // errors are returned for the driver to interpret. Once the slot is
    // connected, the socket's registration persists across waits.
    task<db_socket_wait_result> wait_socket(std::size_t index, db_slot_socket::native_socket_type native,
        db_socket_readiness wanted, std::optional<std::chrono::milliseconds> wait_timeout,
        const operation_timeout& operation_deadline);
    [[nodiscard]] db_error timed_out() const;
    [[nodiscard]] db_error closing_error() const;

private:
    template <typename result_type>
    using statement_hook_type = task<result_type> (*)(db_pool&, std::size_t, std::string_view,
        std::span<const db_value>, std::pmr::memory_resource*, const operation_timeout&);

    task<void> connect_slot(std::size_t index, const operation_timeout& timeout);
    task<void> ready_slot(std::size_t index, const operation_timeout& timeout);
    template <typename result_type>
    task<result_type> run_leased(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options, statement_hook_type<result_type> statement);
    template <typename result_type>
    task<result_type> run_on_transaction_slot(std::size_t index, std::pmr::string sql,
        std::pmr::vector<db_value> params, std::pmr::memory_resource* resource, const operation_options& options,
        statement_hook_type<result_type> statement);
    task<void> execute_control(std::size_t index, std::string_view sql, std::pmr::memory_resource* resource,
        const operation_timeout& timeout);
    task<void> finish_transaction(std::size_t index, std::string_view command,
        std::pmr::memory_resource* resource, const operation_options& options);

    asio::io_context& io_context_;
    db_config_storage config_;
    std::pmr::memory_resource* resource_;
    const worker_handle& worker_;
    // Declared before the lease pool: pending deadline timers address slots.
    std::pmr::vector<slot_type> slots_;
    leases_type leases_;
};

#ifdef RUVIA_ENABLE_MARIADB
extern template class db_pool<mariadb_driver>;
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
extern template class db_pool<postgresql_driver>;
#endif

}  // namespace ruvia::detail
