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
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_backend.h"

#include "db/db_config_storage.h"
#include "db/db_host_resolution.h"
#include "db/db_slot_socket.h"
#include "db/postgresql_pipeline.h"
#include "integration/slot_pool.h"

struct st_mysql;
struct pg_conn;

// One worker-local database pool, shared by every driver. The pool owns lease
// scheduling, cancellation, deadlines, connection lifecycle, and the statement,
// stream and transaction shapes; a driver supplies its connection type, the
// protocol hooks that talk to its client library, and the state it keeps per
// connection (session_type) and per pool (state_type).

namespace ruvia::detail {

enum class db_deadline_kind : std::uint8_t {
    resolve,
    socket,
    // A driver-requested timed pause without a socket wait (MariaDB connect).
    sleep,
};

// What a driver hook reports after starting an unbuffered statement.
struct db_stream_start final {
    explicit db_stream_start(std::pmr::memory_resource* resource = nullptr)
        : column_names_(pmr_resource_or_default(resource)) {}

    // Driver-owned result handle handed to the stream (may be null).
    void* result_{nullptr};
    std::pmr::vector<std::pmr::string> column_names_;
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
// NUL-terminated, and string parameters own their NUL-terminated storage for
// the entire hook call. Hooks throw on failure. A statement_failed db_error or a
// std::invalid_argument promises that the command has been drained and the
// connection is idle -- a hook that cannot get there closes the slot before
// throwing -- so the pool keeps the connection; any other failure closes it.
//
// state_type may keep a lease that release_slot() gives back for a caller of
// its own (take_released_slot() returns true), hears when a caller stops
// waiting for a lease, and is closed by close_now(). A driver
// whose pipelines_statements is true runs query() and execute() outside
// transactions through its own query_pipelined() and execute_pipelined()
// instead of leasing a slot per statement.
struct mariadb_driver final {
    using connection_type = st_mysql;
    using pool_type = db_pool<mariadb_driver>;
    // Statements are sent as text and lease their connection one at a time;
    // neither a connection nor the pool keeps statement state.
    struct session_type final {
        explicit session_type(std::pmr::memory_resource*) noexcept {}
    };
    struct state_type final {
        state_type(pool_type&, const worker_handle&) noexcept {}
        [[nodiscard]] bool take_released_slot(std::size_t) noexcept {
            return false;
        }
        void exclusive_waiter_left() noexcept {}
        void close() noexcept {}
    };
    static constexpr bool pipelines_statements = false;
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
        std::pmr::vector<db_field>& fields, std::pmr::vector<std::pmr::string>& column_names,
        std::pmr::memory_resource* resource, const operation_options& options);
    static task<void> close_stream(pool_type& pool, std::size_t slot, void* result,
        std::pmr::memory_resource* resource, const operation_options& options);
    static void discard_stream(void* result) noexcept;
};

struct postgresql_driver final {
    using connection_type = pg_conn;
    using pool_type = db_pool<postgresql_driver>;
    using session_type = postgresql_session;
    using state_type = postgresql_pipeline_state;
    static constexpr bool pipelines_statements = true;
    static constexpr db_driver driver = db_driver::postgresql;
    static constexpr std::string_view name{"PostgreSQL"};

    static task<db_rows> query_pipelined(pool_type& pool, std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options, operation_scope* scope);
    static task<db_exec_result> execute_pipelined(pool_type& pool, std::pmr::string sql,
        std::pmr::vector<db_value> params, std::pmr::memory_resource* resource, operation_options options,
        operation_scope* scope);
    static task<void> connect(pool_type& pool, std::size_t slot, const operation_timeout& deadline);
    static void close_connection(connection_type& connection) noexcept;
    static task<db_rows> query(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline);
    static task<db_exec_result> execute(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline);
    static task<db_stream_start> start_stream(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, std::pmr::memory_resource* resource, const operation_timeout& deadline);
    static task<std::optional<db_row>> read_stream_row(pool_type& pool, std::size_t slot, void* result,
        std::pmr::vector<db_field>& fields, std::pmr::vector<std::pmr::string>& column_names,
        std::pmr::memory_resource* resource, const operation_options& options);
    static task<void> close_stream(pool_type& pool, std::size_t slot, void* result,
        std::pmr::memory_resource* resource, const operation_options& options);
    static void discard_stream(void* result) noexcept;
};

// One pooled connection. The driver owns the native socket; db_slot_socket
// waits on a Ruvia-owned duplicate of it.
template <typename driver_type>
struct db_slot final {
    db_slot(asio::io_context& io_context, std::pmr::memory_resource* resource)
        : resolver_(io_context),
          wait_socket_(io_context),
          session_(resource) {}

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
    // Driver state of the current connection.
    [[no_unique_address]] typename driver_type::session_type session_;
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
        [[noreturn]] static void throw_acquire_failure(const db_pool& pool, worker_wait_status status);
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
    // Outside a transaction, a driver that pipelines statements shares
    // connections between query() and execute() callers.
    task<db_rows> query(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options,
        operation_scope* scope = nullptr);
    task<db_exec_result> execute(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options,
        operation_scope* scope = nullptr);
    task<db_stream_result> stream(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options);
    task<std::optional<db_row>> read_stream_row(std::size_t slot, void* result,
        std::pmr::vector<db_field>& fields, std::pmr::vector<std::pmr::string>& column_names,
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
    // Lease services for driver hooks. A driver never waits for a lease: the
    // pool's own waits (lease_acquire) are the only callers the lease scheduler
    // queues, so arrival order covers every one of them.
    [[nodiscard]] bool closing() const noexcept {
        return leases_.closing();
    }
    void throw_if_aborted(std::size_t index) const {
        leases_.throw_if_aborted(index);
    }
    // Starts an operation on a leased slot: clears a cancellation its previous
    // operation left, while closing stays permanent.
    void reset_abort(std::size_t index) noexcept {
        leases_.reset_abort(index);
    }
    // Leases a free slot without waiting; nullopt while every slot is leased
    // or the pool is closing.
    [[nodiscard]] std::optional<std::size_t> try_lease() {
        return leases_.try_acquire();
    }
    // Closes the leased slot when `stop` is requested, until finished.
    [[nodiscard]] typename leases_type::cancellation_guard cancel_on_stop(std::size_t index, const stop_token& stop) {
        return typename leases_type::cancellation_guard(leases_, index, stop);
    }
    [[nodiscard]] typename driver_type::state_type& state() noexcept {
        return state_;
    }
    // Arrival order shared by every caller that waits for a connection: a
    // driver stamps its own waiters with it to keep their order against the
    // callers waiting for a lease.
    [[nodiscard]] std::uint64_t next_arrival() noexcept {
        return ++arrivals_;
    }
    // The arrival of the oldest caller still waiting for a lease, if any. One
    // whose stop was requested no longer counts: the lease scheduler drops it
    // at once, while the resumption that unlists it may come a turn later.
    [[nodiscard]] std::optional<std::uint64_t> oldest_exclusive_arrival() const noexcept;
    // Gives a lease back, unless the driver state keeps it for a caller of its
    // own. Every lease this pool hands out ends here.
    void release_slot(std::size_t index) noexcept;
    // Connects a leased slot unless it is connected; a failure closes it.
    task<void> connect_slot(std::size_t index, const operation_timeout& timeout);
    // Closes the slot's connection now, or -- while a wait still borrows it --
    // wakes that wait so the driving coroutine finishes the close.
    void close_slot(std::size_t index) noexcept;
    task<db_resolved_addresses_type> resolve_host(std::size_t index, const operation_timeout& deadline);
    // One wait on a leased slot: readiness of the driver's current socket, or
    // a timed pause without a socket (MariaDB connect), under an optional
    // per-wait timeout. Not a coroutine: await_ready() registers the socket and
    // arms the slot deadline, await_suspend() hands the coroutine to the
    // readiness wait or the deadline, and await_resume() classifies the
    // outcome -- so a wait allocates nothing. Cancellation, closing, operation
    // timeout, and wait failures are thrown as db_error; slot-deadline expiry
    // and socket errors are returned for the driver to interpret, and a pause
    // always returns expired. Once the slot is connected, the socket's
    // registration persists across waits.
    class slot_wait final {
    public:
        slot_wait(db_pool& pool, std::size_t index, db_deadline_kind kind, db_slot_socket::native_socket_type native,
            db_socket_readiness wanted, std::optional<std::chrono::milliseconds> wait_timeout,
            const operation_timeout& operation_deadline) noexcept;
        ~slot_wait();

        slot_wait(const slot_wait&) = delete;
        slot_wait& operator=(const slot_wait&) = delete;

        [[nodiscard]] bool await_ready();
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept;
        [[nodiscard]] db_socket_wait_result await_resume();

    private:
        db_pool& pool_;
        std::size_t index_;
        db_deadline_kind kind_;
        db_slot_socket::native_socket_type native_;
        std::optional<std::chrono::milliseconds> wait_timeout_;
        const operation_timeout& operation_deadline_;
        db_slot_socket::readiness_awaiter socket_wait_;
        // The slot is marked borrowed between arming and resumption.
        bool borrowed_{false};
    };

    [[nodiscard]] slot_wait wait_socket(std::size_t index, db_slot_socket::native_socket_type native,
        db_socket_readiness wanted, std::optional<std::chrono::milliseconds> wait_timeout,
        const operation_timeout& operation_deadline) noexcept {
        return slot_wait(*this, index, db_deadline_kind::socket, native, wanted, wait_timeout, operation_deadline);
    }
    [[nodiscard]] slot_wait sleep(std::size_t index, std::optional<std::chrono::milliseconds> wait_timeout,
        const operation_timeout& operation_deadline) noexcept {
        return slot_wait(*this, index, db_deadline_kind::sleep, db_slot_socket::invalid_socket,
            db_socket_readiness::none, wait_timeout, operation_deadline);
    }
    [[nodiscard]] db_error timed_out() const;
    [[nodiscard]] db_error closing_error() const;

private:
    template <typename result_type>
    using statement_hook_type = task<result_type> (*)(db_pool&, std::size_t, std::string_view,
        std::span<const db_value>, std::pmr::memory_resource*, const operation_timeout&);

    // A caller waiting for a lease, listed in arrival order for the duration
    // of its acquire.
    class exclusive_wait final {
    public:
        exclusive_wait(db_pool& pool, const stop_token& stop) noexcept;
        exclusive_wait(const exclusive_wait&) = delete;
        exclusive_wait& operator=(const exclusive_wait&) = delete;
        ~exclusive_wait();

    private:
        friend class db_pool;

        db_pool& pool_;
        const stop_token& stop_;
        std::uint64_t arrival_;
        exclusive_wait* previous_;
        exclusive_wait* next_{nullptr};
    };

    // The pool's only wait for a lease: stamped and listed before the lease
    // scheduler queues it, unlisted once its acquire returns. The awaitable
    // lives for its co_await expression, so it adds no coroutine frame.
    class lease_acquire final {
    public:
        lease_acquire(db_pool& pool, const operation_timeout& timeout, const stop_token& stop)
            : waiting_(pool, stop),
              acquire_(pool.leases_.acquire(timeout, pool.config_.acquire_timeout_, stop).operator co_await()) {}
        lease_acquire(const lease_acquire&) = delete;
        lease_acquire& operator=(const lease_acquire&) = delete;

        [[nodiscard]] bool await_ready() const noexcept {
            return acquire_.await_ready();
        }
        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
            return acquire_.await_suspend(continuation);
        }
        [[nodiscard]] std::size_t await_resume() {
            return acquire_.await_resume();
        }

    private:
        exclusive_wait waiting_;
        task_awaiter<std::size_t> acquire_;
    };

    // Gives a lease back through release_slot(), so the driver state sees it,
    // on scope exit.
    class pooled_lease final {
    public:
        pooled_lease(db_pool& pool, std::size_t index) noexcept
            : pool_(pool),
              index_(index) {}
        pooled_lease(const pooled_lease&) = delete;
        pooled_lease& operator=(const pooled_lease&) = delete;
        ~pooled_lease() {
            pool_.release_slot(index_);
        }

    private:
        db_pool& pool_;
        std::size_t index_;
    };

    template <typename result_type>
    task<result_type> run_leased(std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, operation_options options, statement_hook_type<result_type> statement,
        operation_scope* scope = nullptr);
    template <typename result_type>
    task<result_type> run_on_transaction_slot(std::size_t index, std::pmr::string sql,
        std::pmr::vector<db_value> params, std::pmr::memory_resource* resource, const operation_options& options,
        statement_hook_type<result_type> statement);
    task<void> execute_control(std::size_t index, std::string_view sql, std::pmr::memory_resource* resource,
        const operation_timeout& timeout);
    task<void> finish_transaction(std::size_t index, std::string_view command,
        std::pmr::memory_resource* resource, const operation_options& options);
    task<void> retire_failed_transaction_slot(std::size_t index, std::pmr::memory_resource* resource,
        const operation_timeout& timeout, const std::exception_ptr& failure);

    asio::io_context& io_context_;
    db_config_storage config_;
    std::pmr::memory_resource* resource_;
    const worker_handle& worker_;
    // Declared before the lease pool: pending deadline timers address slots.
    std::pmr::vector<slot_type> slots_;
    leases_type leases_;
    std::uint64_t arrivals_{0};
    exclusive_wait* oldest_exclusive_{nullptr};
    exclusive_wait* newest_exclusive_{nullptr};
    // Declared after the lease pool, whose leases it borrows.
    [[no_unique_address]] typename driver_type::state_type state_;
};

#ifdef RUVIA_ENABLE_MARIADB
extern template class db_pool<mariadb_driver>;
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
extern template class db_pool<postgresql_driver>;
#endif

}  // namespace ruvia::detail
