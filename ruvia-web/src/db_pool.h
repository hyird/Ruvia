#pragma once

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/web/db/db_session.h"

#include "client_tcp_transport.h"
#include "db_pipeline.h"
#include "operation_interrupt.h"
struct st_mysql;
struct pg_conn;

// One worker-local database pool, shared by every driver. The pool owns lease
// scheduling, cancellation, deadlines, connection lifecycle, and the statement,
// stream and transaction shapes; a driver supplies its connection type, the
// protocol hooks that talk to its client library, and the state it keeps per
// connection (session_type) and per pool (state_type).

#include <string>

#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_query.h"
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_session.h"
#endif

#include <algorithm>
#include <type_traits>

#include <asio/error.hpp>

#include "ruvia/core/event_loop.h"

#include "db_pipeline.h"
namespace ruvia::detail {

using db_resolved_addresses_type = std::vector<std::string>;

[[nodiscard]] db_resolved_addresses_type collect_db_resolved_addresses(
    const client_resolve_result& results, db_driver driver);

// Connector/C accepts comma-separated hosts and performs failover only while
// opening the transport. IPv6 literals in a multi-host list must be bracketed
// so their colons are not parsed as a per-host port separator; a lone IPv6
// literal remains unbracketed because Connector/C only invokes that list parser
// when a comma is present.
[[nodiscard]] std::string make_mariadb_resolved_host_list(
    std::span<const std::string> addresses);

struct postgresql_resolved_host_list final {
    std::string hosts_;
    std::string addresses_;
};

// libpq requires host and hostaddr lists to contain the same number of items.
// Repeating the logical hostname preserves TLS/GSS/password-file identity while
// the numeric hostaddr list suppresses libpq's blocking DNS lookup.
[[nodiscard]] postgresql_resolved_host_list make_postgresql_resolved_host_list(
    std::string_view logical_host, std::span<const std::string> addresses);

enum class db_deadline_kind : std::uint8_t {
    resolve,
    socket,
    // A driver-requested timed pause without a socket wait (MariaDB connect).
    sleep,
};

// What a driver hook reports after starting an unbuffered statement.
struct db_stream_start final {
    db_stream_start() = default;

    // Driver-owned result handle handed to the stream (may be null).
    void* result_{nullptr};
    std::vector<std::string> column_names_;
    // False when the statement produced no row set; the slot is still clean.
    bool returns_rows_{true};
};

// How a stream ended. Either way the driver has read the statement to its end,
// released its result handle, and left the connection idle.
enum class db_stream_end : std::uint8_t {
    exhausted,
    // The statement produced no row set, which only its first result tells.
    no_row_set,
};

// One read of a stream: its next row, or how it ended.
using db_stream_step = std::variant<db_row, db_stream_end>;

template <typename driver_type>
class db_pool;

// The configured port as a NUL-terminated buffer, the form both asio's
// resolver and libpq take it in.
[[nodiscard]] std::array<char, 6> format_db_port(std::uint16_t port);

// The driver hooks. Statement hooks run on a connected, leased slot under a
// deadline already constrained by the query timeout and, on a transaction or
// stream lease, by the lease deadline. `sql` views are
// NUL-terminated and contain no other NUL, and string parameters own their
// NUL-terminated storage for the entire hook call: raw SQL is checked once
// where it enters (prepare_db_statement), compiled SQL is built that way, and
// parameters are checked where they are encoded for the client library. Hooks
// throw on failure. A statement_failed db_error or a std::invalid_argument
// promises that the command has been drained and the connection is idle -- a
// hook that cannot get there closes the slot before throwing -- so the pool
// keeps the connection; any other failure closes it.
//
// Outside transactions, query() and execute() run through the driver's
// pipeline (pipeline_io_type, db_pipeline.h), which shares connections between
// callers; state_type is that pipeline's per-pool state and session_type keeps
// the group of each connection. The state may keep a lease that release_slot()
// gives back for a caller of its own (take_released_slot() returns true),
// hears when a caller stops waiting for a lease, and is closed by close_now().
// Before any lease is reassigned, reusable_connection() must confirm that the
// drained connection is outside a transaction and safe for unrelated callers.
// idle_connection_alive() looks at a connection that sat in the pool, without
// blocking and before anything is sent on it: false when the server ended the
// session meanwhile, so the pool reconnects instead.
//
// Stream hooks are protocol steps only: the pool owns a stream's cancellation,
// deadline and the release of its slot. A stream's `result` handle is the
// driver's, kept in the slot (db_slot::stream_result_); a hook that frees it
// sets it to null, and after a hook throws the pool discards what is left of
// it (discard_stream()) and closes the slot.
struct mariadb_driver final {
    using connection_type = st_mysql;
    using pool_type = db_pool<mariadb_driver>;
    using session_type = mariadb_session;
    using pipeline_io_type = mariadb_pipeline_io;
    using state_type = db_pipeline_state<pipeline_io_type>;
    static constexpr db_driver driver = db_driver::mariadb;
    static constexpr std::string_view name{"MariaDB"};

    static task<void> connect(pool_type& pool, std::size_t slot, const operation_deadline& deadline);
    static void close_connection(connection_type& connection) noexcept;
    [[nodiscard]] static bool reusable_connection(connection_type& connection) noexcept;
    [[nodiscard]] static bool idle_connection_alive(connection_type& connection) noexcept;
    static task<db_rows> query(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, const operation_deadline& deadline);
    static task<db_exec_result> execute(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, const operation_deadline& deadline);
    static task<db_stream_start> start_stream(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, const operation_deadline& deadline);
    static task<db_stream_step> read_stream_row(pool_type& pool, std::size_t slot, void*& result,
        std::vector<db_field>& fields, std::vector<std::string>& column_names, const operation_deadline& deadline);
    // Ends a stream before its rows ran out; on return the connection is idle.
    static task<void> close_stream(pool_type& pool, std::size_t slot, void*& result, const operation_deadline& deadline);
    static void discard_stream(void* result) noexcept;
};

struct postgresql_driver final {
    using connection_type = pg_conn;
    using pool_type = db_pool<postgresql_driver>;
    using session_type = postgresql_session;
    using pipeline_io_type = postgresql_pipeline_io;
    using state_type = db_pipeline_state<pipeline_io_type>;
    static constexpr db_driver driver = db_driver::postgresql;
    static constexpr std::string_view name{"PostgreSQL"};

    static task<void> connect(pool_type& pool, std::size_t slot, const operation_deadline& deadline);
    static void close_connection(connection_type& connection) noexcept;
    [[nodiscard]] static bool reusable_connection(connection_type& connection) noexcept;
    [[nodiscard]] static bool idle_connection_alive(connection_type& connection) noexcept;
    static task<db_rows> query(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, const operation_deadline& deadline);
    static task<db_exec_result> execute(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, const operation_deadline& deadline);
    static task<db_stream_start> start_stream(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, const operation_deadline& deadline);
    static task<db_stream_step> read_stream_row(pool_type& pool, std::size_t slot, void*& result,
        std::vector<db_field>& fields, std::vector<std::string>& column_names, const operation_deadline& deadline);
    // Ends a stream before its rows ran out; on return the connection is idle
    // or closed.
    static task<void> close_stream(pool_type& pool, std::size_t slot, void*& result, const operation_deadline& deadline);
    static void discard_stream(void* result) noexcept;
};

// One pooled connection. The driver owns the native socket; db_slot_socket
// waits on a Ruvia-owned duplicate of it. Pending Asio handlers, deadline
// continuations and pipeline records address the slot, so it never moves.
template <typename driver_type>
struct db_slot final {
    db_slot(asio::io_context& io_context, const worker_ref& worker)
        : resolver_(worker),
          wait_socket_(io_context) {}

    ~db_slot() {
        if (wait_active_) {
            std::terminate();
        }
    }

    db_slot(const db_slot&) = delete;
    db_slot& operator=(const db_slot&) = delete;

    client_host_resolver resolver_;
    typename driver_type::connection_type* connection_{nullptr};
    db_slot_socket wait_socket_;
    // Driver state of the current connection.
    [[no_unique_address]] typename driver_type::session_type session_;
    // A sleep wait parks here until its deadline or a close resumes it.
    std::coroutine_handle<> deadline_continuation_{};
    // The driver result of the stream holding this slot, null otherwise. The
    // pool keeps it so an expiring lease can discard it before closing.
    void* stream_result_{nullptr};
    // Whole-lease deadline (db_config::transaction_timeout_) of the
    // transaction or stream holding this slot; unbounded for any other lease.
    operation_deadline lease_timeout_{std::nullopt};
    bool connected_{false};
    // Shutdown may request closure while an asynchronous wait still borrows
    // this slot and its socket. The transport stays alive until that wait
    // resumes and the driving coroutine performs the final close; destroying
    // it immediately would leave the queued Asio handler dangling.
    bool wait_active_{false};
    bool close_requested_{false};
    // An operation of the transaction or stream holding this slot is running.
    // Its deadline already ends no later than lease_timeout_, so the lease
    // timer leaves it to that deadline.
    bool lease_busy_{false};
    // Fires at lease_timeout_ to close the connection of a lease its holder
    // keeps idle. Declared last so it is unregistered before anything it
    // reaches is destroyed.
    worker_timer_registration lease_timer_;
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

    db_pool(asio::io_context& io_context, const worker_ref& worker_value, db_config_storage config);
    db_pool(asio::io_context&, worker_ref&&, db_config_storage) = delete;
    ~db_pool();

    db_pool(const db_pool&) = delete;
    db_pool& operator=(const db_pool&) = delete;

    [[nodiscard]] task<void> connect();
    void close_now() noexcept;

    // The only execution path for raw SQL, compiled queries and transactions.
    // Outside a transaction, a driver that pipelines statements shares
    // connections between query() and execute() callers. `io_metrics` is the
    // request's counter block, or null outside a request.
    //
    // A transaction (begin_transaction() to commit/rollback) and a stream
    // (stream() until its rows run out or it is closed) each hold one lease
    // under config().transaction_timeout_: every operation on it runs under
    // the earlier of its own deadline and the lease deadline, and a lease
    // whose deadline passes between operations has its connection closed by
    // the lease timer; its next operation fails with timeout and gives the
    // slot back.
    task<db_rows> query(std::string sql, std::vector<db_value> params, operation_options options,
        request_io_metrics* io_metrics = nullptr);
    task<db_exec_result> execute(std::string sql, std::vector<db_value> params, operation_options options,
        request_io_metrics* io_metrics = nullptr);
    task<db_stream_result> stream(std::string sql, std::vector<db_value> params, operation_options options);
    task<std::optional<db_row>> read_stream_row(std::size_t slot, std::vector<db_field>& fields,
        std::vector<std::string>& column_names,
        const operation_options& options);
    task<void> close_stream(std::size_t slot, const operation_options& options);
    void abort_stream(std::size_t slot) noexcept;
    task<db_rows> query_on_transaction_slot(std::size_t slot, std::string sql,
        std::vector<db_value> params, const operation_options& options);
    task<db_exec_result> execute_on_transaction_slot(std::size_t slot, std::string sql,
        std::vector<db_value> params, const operation_options& options);
    task<db_transaction> begin_transaction(operation_options operation_options_value, db_transaction_options transaction_options);
    task<void> commit_transaction(
        std::size_t slot, const operation_options& options);
    task<void> rollback_transaction(
        std::size_t slot, const operation_options& options);
    void abort_transaction(std::size_t slot) noexcept;

    // Services for driver hooks.
    [[nodiscard]] const db_config_storage& config() const noexcept {
        return config_;
    }
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] slot_type& slot(std::size_t index) noexcept {
        return *slots_[index];
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
    // own. Every lease this pool hands out ends here, and so does the lease
    // deadline of a transaction or stream.
    void release_slot(std::size_t index) noexcept;
    // Runs once per new lease, before its first statement: closes the leased
    // slot's pooled connection when the server ended the session while it sat
    // idle, so the next connect_slot() replaces it and nothing is sent on a
    // dead connection.
    void retire_dead_idle_connection(std::size_t index) noexcept;
    // Connects a leased slot unless it is connected; a failure closes it. A
    // pool that does not reconnect (db_config_storage::reconnect_) fails here
    // for a slot whose connection ended.
    task<void> connect_slot(std::size_t index, const operation_deadline& timeout);
    // Closes the slot's connection now, or -- while a wait still borrows it --
    // wakes that wait so the driving coroutine finishes the close.
    void close_slot(std::size_t index) noexcept;
    task<db_resolved_addresses_type> resolve_host(std::size_t index, const operation_deadline& deadline);
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
            db_socket_readiness wanted, std::optional<std::chrono::steady_clock::time_point> wait_deadline,
            const operation_deadline& operation_deadline_value) noexcept;
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
        std::optional<std::chrono::steady_clock::time_point> wait_deadline_;
        const operation_deadline& operation_deadline_;
        db_slot_socket::readiness_awaiter socket_wait_;
        // The slot is marked borrowed between arming and resumption.
        bool borrowed_{false};
    };

    [[nodiscard]] slot_wait wait_socket(std::size_t index, db_slot_socket::native_socket_type native,
        db_socket_readiness wanted, std::optional<std::chrono::steady_clock::time_point> wait_deadline,
        const operation_deadline& operation_deadline_value) noexcept {
        return slot_wait(*this, index, db_deadline_kind::socket, native, wanted, wait_deadline, operation_deadline_value);
    }
    [[nodiscard]] slot_wait sleep(std::size_t index, std::optional<std::chrono::steady_clock::time_point> wait_deadline,
        const operation_deadline& operation_deadline_value) noexcept {
        return slot_wait(*this, index, db_deadline_kind::sleep, db_slot_socket::invalid_socket,
            db_socket_readiness::none, wait_deadline, operation_deadline_value);
    }
    [[nodiscard]] db_error timed_out() const;
    [[nodiscard]] db_error closing_error() const;

private:
    template <typename result_type>
    using statement_hook_type = task<result_type> (*)(db_pool&, std::size_t, std::string_view,
        std::span<const db_value>, const operation_deadline&);

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
        lease_acquire(db_pool& pool, const operation_deadline& timeout, const stop_token& stop)
            : pool_(pool),
              waiting_(pool, stop),
              acquire_(pool.leases_.acquire(timeout, pool.config_.acquire_timeout_, stop)) {}
        lease_acquire(const lease_acquire&) = delete;
        lease_acquire& operator=(const lease_acquire&) = delete;

        [[nodiscard]] bool await_ready() {
            return acquire_.await_ready();
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
            return acquire_.await_suspend(continuation);
        }
        [[nodiscard]] std::size_t await_resume() {
            const auto index = acquire_.await_resume();
            pool_.retire_dead_idle_connection(index);
            return index;
        }

    private:
        db_pool& pool_;
        exclusive_wait waiting_;
        typename leases_type::acquire_awaiter acquire_;
    };

    template <typename result_type>
    task<result_type> run_on_transaction_slot(std::size_t index, std::string sql,
        std::vector<db_value> params, const operation_options& options,
        statement_hook_type<result_type> statement);
    task<void> execute_control(std::size_t index, std::string_view sql,
        const operation_deadline& timeout);
    task<void> finish_transaction(std::size_t index, std::string_view command, const operation_options& options);
    task<void> retire_failed_transaction_slot(std::size_t index,
        const operation_deadline& timeout, const std::exception_ptr& failure);
    task<void> open_slot(std::size_t index, const operation_deadline& timeout);
    // Starts the lease deadline of a transaction or stream on its fresh lease,
    // with its first operation running.
    void start_lease(std::size_t index);
    // Starts one operation on a transaction or stream lease: reports an
    // aborted slot, then a lease whose deadline has passed.
    void enter_lease_operation(std::size_t index);
    // Leaves the lease held and idle after an operation succeeded; throws
    // timeout when the lease deadline passed meanwhile, so the operation fails
    // and its caller retires the lease.
    void leave_lease_operation(std::size_t index);
    // The earlier of an operation's own deadline and its lease deadline.
    [[nodiscard]] operation_deadline lease_operation_timeout(
        std::size_t index, const operation_deadline& timeout) const noexcept;
    // Lease timer expiry: closes an idle lease's connection, so the server
    // ends its transaction and releases its locks while the holder keeps the
    // lease.
    void expire_idle_lease(std::size_t index) noexcept;
    [[nodiscard]] db_error lease_timed_out() const;
    // Discards what a failed stream left, closes its connection and gives its
    // lease back.
    void fail_stream(std::size_t index, typename leases_type::cancellation_guard& cancellation) noexcept;
    // Every lease names a slot of this pool; any other index breaks the lease
    // lifecycle, which nothing can unwind from.
    void require_slot_index(std::size_t index) const noexcept;

    asio::io_context& io_context_;
    db_config_storage config_;
    const worker_ref& worker_;
    // Declared before the lease pool: pending deadline timers address slots.
    // Each slot is allocated once and stays put while the pool lives.
    std::vector<std::unique_ptr<slot_type>> slots_;
    leases_type leases_;
    std::uint64_t arrivals_{0};
    exclusive_wait* oldest_exclusive_{nullptr};
    exclusive_wait* newest_exclusive_{nullptr};
    // Declared after the lease pool, whose leases it borrows.
    [[no_unique_address]] typename driver_type::state_type state_;
};

#ifdef RUVIA_ENABLE_MARIADB
extern template class db_pipeline_state<mariadb_pipeline_io>;
extern template class db_pool<mariadb_driver>;
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
extern template class db_pipeline_state<postgresql_pipeline_io>;
extern template class db_pool<postgresql_driver>;
#endif

// Every pool operation dispatches on the configured backend; reaching the end
// means the build has no driver for it.
[[noreturn]] inline void throw_unavailable_db_backend() {
    throw std::logic_error("database backend is not available");
}

// db_pool_ref_type is a closed backend set. Keep its single checked dispatch here so
// handle, stream, transaction, and registry operations cannot drift into
// subtly different null or unavailable-backend behavior.
template <typename visitor_type>
decltype(auto) visit_db_pool([[maybe_unused]] const db_pool_ref_type& pool, [[maybe_unused]] visitor_type&& visitor) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<db_pool<mariadb_driver>*>(&pool);
        client != nullptr && *client != nullptr) {
        return std::forward<visitor_type>(visitor)(**client);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<db_pool<postgresql_driver>*>(&pool);
        client != nullptr && *client != nullptr) {
        return std::forward<visitor_type>(visitor)(**client);
    }
#endif
    throw_unavailable_db_backend();
}

// Destruction and immediate close paths cannot report an empty backend. They
// deliberately ignore it while preserving the same closed-set dispatch.
template <typename visitor_type>
void visit_db_pool_if_present([[maybe_unused]] const db_pool_ref_type& pool, [[maybe_unused]] visitor_type&& visitor) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<db_pool<mariadb_driver>*>(&pool);
        client != nullptr && *client != nullptr) {
        std::forward<visitor_type>(visitor)(**client);
        return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<db_pool<postgresql_driver>*>(&pool);
        client != nullptr && *client != nullptr) {
        std::forward<visitor_type>(visitor)(**client);
    }
#endif
}

[[nodiscard]] inline db_driver db_pool_driver(const db_pool_ref_type& pool) {
    return visit_db_pool(pool, [](const auto& client) noexcept { return client.config().driver_; });
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Every result shape, so cache-id removal can name each entry of one id.
inline constexpr std::array db_cache_shapes{
    db_cache_shape::rows, db_cache_shape::one, db_cache_shape::exists, db_cache_shape::count};

// The key part naming each result shape, indexed by its value. An id-keyed
// entry stores the caller's id and this tag as separate key parts, so no user
// id can spell another id's shape.
inline constexpr std::array<std::string_view, db_cache_shapes.size()> db_cache_shape_tags{
    "rows", "one", "exists", "count"};

[[nodiscard]] constexpr std::string_view db_cache_shape_tag(db_cache_shape shape) noexcept {
    return db_cache_shape_tags[static_cast<std::size_t>(shape)];
}

// The key prefix of one alias's cache entries: everything that decides what a
// cached result means (namespace, alias, driver, server, database, user and
// TLS server name) digested into one key part.
[[nodiscard]] std::string db_cache_prefix(std::string_view name_space, std::string_view alias,
    const db_config_storage& config);

[[nodiscard]] std::string encode_db_cache_rows(const db_rows& rows);
[[nodiscard]] db_rows decode_db_cache_rows(std::string bytes_value);
// Key functions take the immutable prefix already built for this cache owner.
// Append all result shapes for one id, sharing the id digest.
void append_db_cache_id_keys(std::vector<std::string>& keys, std::string_view prefix, std::string_view id);
// Keyed by id and shape when the query names an id, otherwise by its SQL and parameters.
[[nodiscard]] std::string db_cache_key(std::string_view prefix, std::string_view id, db_cache_shape shape,
    std::string_view sql, std::span<const db_value> params, db_driver driver);

// The deferred database statement behind a cached step. On a cache hit it is
// destroyed without starting; on a miss it runs once on the plan's backend.
class db_cache_query final {
public:
    db_cache_query(const db_query_backend& backend, std::string sql, std::vector<db_value> params) noexcept
        : backend_(backend),
          sql_(std::move(sql)),
          params_(std::move(params)) {}

    db_cache_query(const db_cache_query&) = delete;
    db_cache_query& operator=(const db_cache_query&) = delete;
    db_cache_query(db_cache_query&&) noexcept = default;
    db_cache_query& operator=(db_cache_query&&) = delete;

    // The returned task owns the statement and the options it runs under.
    [[nodiscard]] task<db_rows> operator()(operation_options options) &&;

private:
    db_query_backend backend_;
    std::string sql_;
    std::vector<db_value> params_;
};

// One database alias's Redis-backed query-result cache (Redis builds only).
// Worker-local: concurrent misses of one key on this worker share one fill.
#ifdef RUVIA_ENABLE_REDIS
class db_query_cache_state final {
public:
    // `prefix` is the alias's db_cache_prefix().
    db_query_cache_state(const redis_session& redis, const db_cache_config_storage& config, std::string prefix);
    ~db_query_cache_state();
    void close_now() noexcept;
    [[nodiscard]] std::optional<std::string> key(
        const db_query& query, const db_statement& statement, db_driver driver);
    // Serves the step from the cache, or runs `database` and stores its rows.
    [[nodiscard]] task<db_rows> wrap(std::optional<std::chrono::milliseconds> duration, std::string key,
        db_cache_query database, operation_options options, operation_deadline deadline);
    // `io_metrics` is the request's counter block, or null outside a request.
    [[nodiscard]] task<void> remove(
        std::span<const std::string_view> ids, request_io_metrics* io_metrics, operation_options options);
    [[nodiscard]] task<void> clear(request_io_metrics* io_metrics, operation_options options);

private:
    // One read-through fill in flight: the caller that missed first (the
    // leader) runs the statement; later misses of the same full key wait for
    // its encoded result, or for its end without one, and then look again.
    // Shared, so a waiter still reads it after the leader's frame is gone.
    struct fill final {
        explicit fill(std::string_view key)
            : key_(key) {}

        std::string key_;
        // Requested once the fill ends, with or without a result.
        stop_source done_;
        std::optional<std::string> result_;
        // A waiter joined, so the leader publishes its result in memory.
        bool awaited_{false};
    };
    using fill_pointer = std::shared_ptr<fill>;
    class fill_lead;

    void require_open() const;
    task<db_rows> query(std::string key, std::chrono::milliseconds duration, db_cache_query database,
        operation_options options, operation_deadline deadline);
    // Ends the fill if it is still the one listed for its key. Later misses
    // of that key no longer join it.
    void unlist_fill(const fill& ended) noexcept;
    // After remove() or clear(), a fill that started before must not answer
    // callers that arrive after: they read the database themselves.
    void unlist_fills(std::span<const std::string> keys) noexcept;
    void unlist_all_fills() noexcept;

    bool closed_{false};
    stop_source stop_;
    const worker_ref& worker_;
    std::chrono::milliseconds duration_;
    bool always_enabled_;
    bool ignore_errors_;
    // The store's reply limit: a GET of a larger entry fails on every hit, so
    // such an entry is never written.
    std::optional<std::size_t> max_entry_reply_bytes_;
    std::string prefix_;
    redis_session redis_;
    std::vector<fill_pointer> fills_;
};
#endif

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)

namespace ruvia {
class redis_session;
}

namespace ruvia::detail {

class redis_registry;
struct request_io_metrics;
#ifdef RUVIA_ENABLE_REDIS
class db_query_cache_state;
#endif

class db_registry final {
public:
    db_registry(asio::io_context& io_context, const worker_ref& worker_value,
        const db_config& default_config);
    db_registry(asio::io_context& io_context, const worker_ref& worker_value,
        std::span<const db_definition> databases, redis_registry* redis = nullptr);
#ifdef RUVIA_ENABLE_REDIS
    db_registry(asio::io_context& io_context, const worker_ref& worker_value,
        const db_config& config,
        const redis_session& redis, const db_cache_config& policy);
#endif
    ~db_registry();

    db_registry(const db_registry&) = delete;
    db_registry& operator=(const db_registry&) = delete;

    task<void> connect();
    void close_now() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    // `io_metrics` is the request's counter block, or null outside a request.
    [[nodiscard]] db_session get(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;
    [[nodiscard]] db_session get(
        std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;

    template <typename driver_type>
    using pool_owner_ptr_type = std::unique_ptr<db_pool<driver_type>>;
#if defined(RUVIA_ENABLE_MARIADB) && defined(RUVIA_ENABLE_POSTGRESQL)
    using pool_owner_type = std::variant<pool_owner_ptr_type<mariadb_driver>, pool_owner_ptr_type<postgresql_driver>>;
#elif defined(RUVIA_ENABLE_MARIADB)
    using pool_owner_type = std::variant<pool_owner_ptr_type<mariadb_driver>>;
#else
    using pool_owner_type = std::variant<pool_owner_ptr_type<postgresql_driver>>;
#endif

private:
    void add(asio::io_context& io_context, const worker_ref& worker, db_config_storage config);

    struct entry_type final {
        explicit entry_type(pool_owner_type pool) noexcept;
        pool_owner_type pool_;
#ifdef RUVIA_ENABLE_REDIS
        std::unique_ptr<db_query_cache_state> cache_;
#endif
    };

#ifdef RUVIA_ENABLE_REDIS
    void attach_cache(std::size_t index, const worker_ref& worker_value,
        const redis_session& redis, const db_cache_config_storage& policy,
        const db_config_storage& config, std::string_view alias);
#endif
    [[nodiscard]] db_session session(
        const entry_type& entry, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;

    std::vector<entry_type> entries_;
    named_capability_index alias_index_;
#ifdef RUVIA_ENABLE_REDIS
    mutable ::ruvia::operation_scope cache_scope_;
#endif
};

}  // namespace ruvia::detail

#endif

// Definitions of db_pipeline.h, included after db_pool.h by the driver
// translation units that instantiate a pipeline and by db_pool.cpp, which runs
// statements through it (run_pipelined).
//
// The io_type hooks, all static, each on a connected, leased slot of a group:
//   statement_type prepare(std::string_view sql, std::span<const db_value>)
//       Validates and prepares what queue() needs, before admission.
//   void open(state_type&, std::size_t slot)
//       Readies a connection for a new group; a failure closes it.
//   bool queue(state_type&, std::size_t slot, member_type&, statement_type&, flight_type&)
//       Queues the member's flight. A failure that leaves the protocol out of
//       step fails the group before it is thrown. True when a waiting reader
//       must look again.
//   bool advance(state_type&, std::size_t slot, member_type& reader)
//       One step without waiting: sends, reads or finishes a flight, handing
//       a finished one to complete_head(). False when it needs a wait first.
//   db_socket_readiness wanted(pool_type&, std::size_t slot)
//       What the next wait waits for; none pauses the reader for one worker
//       turn instead, after which advance() is asked again.
//   db_slot_socket::native_socket_type socket(pool_type&, std::size_t slot)
//       The connection's socket for that wait; throws when it has none.
//   void exchange(state_type&, std::size_t slot, db_socket_readiness ready)
//       Continues the protocol after a readiness wake.
//   bool reusable(state_type&, std::size_t slot) noexcept
//       Whether a dissolving group's protocol exchange is complete, with no
//       unread results. The pool checks transaction state before reassigning it.
// group_state has reset(), called when its group's flights are cleared.

namespace ruvia::detail {

// db_config's read and write timeouts bound waiting without socket progress in
// that direction; waiting while output is pending waits to write.
[[nodiscard]] inline std::optional<timeout_duration> db_io_timeout(
    const db_config_storage& config, db_socket_readiness wanted) noexcept {
    return has_db_socket_readiness(wanted, db_socket_readiness::write) ? config.write_timeout_ : config.read_timeout_;
}

// A socket error ends the connection. A cancelled wait does not: it expired,
// or it was woken to be armed again.
inline void check_db_socket_wait(const db_socket_wait_result& result, db_driver driver, std::string_view name) {
    if (result.error_ && result.error_ != asio::error::operation_aborted) {
        throw db_error(db_error::code_type::io_error, driver,
            std::string(name).append(" socket wait failed: ").append(error_code_text(result.error_)),
            result.error_.value());
    }
}

// One socket wait on a slot held exclusively by a driver operation, bounded by
// the operation deadline and optional driver I/O timeout.
template <typename driver_type>
struct db_driver_socket_wait final {
    using pool_type = db_pool<driver_type>;
    pool_type& pool_;
    typename pool_type::slot_wait wait_;
    bool throw_on_expiry_{true};

    [[nodiscard]] bool await_ready() {
        return wait_.await_ready();
    }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept {
        return wait_.await_suspend(handle);
    }
    [[nodiscard]] db_socket_wait_result resume_raw() {
        const auto wait_result = wait_.await_resume();
        if (wait_result.expired_ && throw_on_expiry_) {
            throw pool_.timed_out();
        }
        return wait_result;
    }
    void await_resume() {
        check_db_socket_wait(resume_raw(), driver_type::driver, driver_type::name);
    }
};

template <typename io_type>
db_pipeline_member<io_type>::db_pipeline_member(
    state_type& state, db_rows* rows, const stop_token& stop_token_value)
    : state_(state),
      rows_(rows),
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
    ++state_.statements_;
}

template <typename io_type>
db_pipeline_member<io_type>::~db_pipeline_member() {
    state_.retire(*this);
}

template <typename io_type>
db_pipeline_state<io_type>::db_pipeline_state(pool_type& pool, const worker_ref& worker_value)
    : pool_(pool),
      worker_(worker_value),
      cancellation_target_(make_worker_cancellation_target(*this, worker_value)) {}

template <typename io_type>
db_pipeline_state<io_type>::~db_pipeline_state() {
    cancellation_target_->detach(*this);
    // Started statements complete before their pool is destroyed.
    if (members_ != nullptr || parked_head_ != nullptr) {
        std::terminate();
    }
}

template <typename io_type>
typename db_pipeline_state<io_type>::group_type& db_pipeline_state<io_type>::group(std::size_t slot) const noexcept {
    return pool_.slot(slot).session_.pipeline_;
}

template <typename io_type>
db_error db_pipeline_state<io_type>::cancelled() const {
    return db_error(db_error::code_type::cancelled, driver_type::driver, "database operation cancelled");
}

template <typename io_type>
bool db_pipeline_state<io_type>::may_share(std::uint64_t arrival) const noexcept {
    const auto oldest = pool_.oldest_exclusive_arrival();
    return !oldest.has_value() || arrival < *oldest;
}

// A resume from inside the reader, a stop callback or a timer would re-enter
// the woken statement; it is deferred to the worker instead. Taking the
// continuation makes each suspension resumable exactly once.
template <typename io_type>
void db_pipeline_state<io_type>::settle(member_type& member, db_pipeline_event event) noexcept {
    member.event_ = event;
    if (auto continuation = std::exchange(member.continuation_, {})) {
        worker_.defer_resume(continuation);
    }
}

template <typename io_type>
std::optional<std::size_t> db_pipeline_state<io_type>::roomiest_group() const noexcept {
    std::optional<std::size_t> roomiest;
    std::size_t lowest_load = db_pipeline_depth;
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

template <typename io_type>
void db_pipeline_state<io_type>::join(member_type& member, std::size_t slot) noexcept {
    auto& joined = group(slot);
    ++joined.members_;
    ++joined.reserved_;
    member.slot_ = slot;
    member.phase_ = db_pipeline_phase::reserved;
}

template <typename io_type>
db_pipeline_event db_pipeline_state<io_type>::place(member_type& member) {
    if (!may_share(member.arrival_)) {
        return db_pipeline_event::none;
    }
    if (const auto slot = pool_.try_lease()) {
        member.slot_ = *slot;
        member.phase_ = db_pipeline_phase::leased;
        return db_pipeline_event::leased;
    }
    if (const auto slot = roomiest_group()) {
        join(member, *slot);
        return db_pipeline_event::joined;
    }
    return db_pipeline_event::none;
}

// Parked statements go first, so an arrival only places itself when nobody
// is parked.
template <typename io_type>
db_pipeline_event db_pipeline_state<io_type>::admit(member_type& member, const operation_deadline& admission_deadline) {
    if (pool_.closing()) {
        throw pool_.closing_error();
    }
    if (member.stopped()) {
        throw cancelled();
    }
    member.arrival_ = pool_.next_arrival();
    if (parked_head_ == nullptr) {
        if (const auto event = place(member); event != db_pipeline_event::none) {
            return event;
        }
    }
    park(member, admission_deadline);
    return db_pipeline_event::none;
}

template <typename io_type>
void db_pipeline_state<io_type>::admitted(db_pipeline_event event) const {
    switch (event) {
        case db_pipeline_event::leased:
        case db_pipeline_event::joined:
            return;
        case db_pipeline_event::timed_out:
            pool_type::slot_policy::throw_acquire_failure(pool_, worker_wait_status::timed_out);
        case db_pipeline_event::cancelled:
            throw cancelled();
        case db_pipeline_event::closing:
            throw pool_.closing_error();
        default:
            std::terminate();
    }
}

template <typename io_type>
void db_pipeline_state<io_type>::park(member_type& member, const operation_deadline& admission_deadline) {
    if (const auto deadline_value = admission_deadline.deadline()) {
        if (*deadline_value <= std::chrono::steady_clock::now()) {
            pool_type::slot_policy::throw_acquire_failure(pool_, worker_wait_status::timed_out);
        }
        // A stopped timer queue arms nothing; the statement still leaves
        // through admission, its stop token or close().
        (void)worker_.schedule_timer(
            member.admission_timer_, *deadline_value, [this, &member](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired && member.phase_ == db_pipeline_phase::parked) {
                    unpark(member);
                    member.phase_ = db_pipeline_phase::admitting;
                    settle(member, db_pipeline_event::timed_out);
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
    member.phase_ = db_pipeline_phase::parked;
}

// Leaves the phase to the caller, which knows where the statement goes next.
template <typename io_type>
void db_pipeline_state<io_type>::unpark(member_type& member) noexcept {
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

template <typename io_type>
void db_pipeline_state<io_type>::dispatch() noexcept {
    while (parked_head_ != nullptr && !pool_.closing()) {
        auto& head = *parked_head_;
        const auto event = place(head);
        if (event == db_pipeline_event::none) {
            return;
        }
        unpark(head);
        settle(head, event);
    }
}

// A released connection goes to the oldest parked statement unless an older
// caller waits for it in the lease scheduler. Room would have admitted that
// statement already.
template <typename io_type>
bool db_pipeline_state<io_type>::take_released_slot(std::size_t slot) noexcept {
    if (parked_head_ == nullptr || pool_.closing() || !may_share(parked_head_->arrival_)) {
        return false;
    }
    auto& head = *parked_head_;
    unpark(head);
    head.slot_ = slot;
    head.phase_ = db_pipeline_phase::leased;
    settle(head, db_pipeline_event::leased);
    return true;
}

template <typename io_type>
void db_pipeline_state<io_type>::exclusive_waiter_left() noexcept {
    dispatch();
}

template <typename io_type>
void db_pipeline_state<io_type>::close() noexcept {
    while (parked_head_ != nullptr) {
        auto& head = *parked_head_;
        unpark(head);
        head.phase_ = db_pipeline_phase::admitting;
        settle(head, db_pipeline_event::closing);
    }
}

// Every lease a statement founds a group on passes here first.
template <typename io_type>
bool db_pipeline_state<io_type>::must_connect(member_type& member) {
    pool_.reset_abort(member.slot_);
    if (member.stopped()) {
        throw cancelled();
    }
    pool_.retire_dead_idle_connection(member.slot_);
    member.connecting_ = !pool_.slot(member.slot_).connected_;
    return member.connecting_;
}

// A stop while connecting closed the slot, which ended the connect.
template <typename io_type>
void db_pipeline_state<io_type>::connected(member_type& member, const std::exception_ptr& failure) {
    member.connecting_ = false;
    if (failure == nullptr) {
        return;
    }
    if (member.stopped()) {
        throw cancelled();
    }
    std::rethrow_exception(failure);
}

template <typename io_type>
void db_pipeline_state<io_type>::found_group(member_type& member) {
    io_type::open(*this, member.slot_);
    auto& founded = group(member.slot_);
    founded.members_ = 1;
    founded.reserved_ = 1;
    member.phase_ = db_pipeline_phase::reserved;
    dispatch();
}

template <typename io_type>
void db_pipeline_state<io_type>::require_usable(std::size_t slot) const {
    const auto& checked = group(slot);
    if (checked.closing_) {
        std::rethrow_exception(checked.failure_);
    }
    pool_.throw_if_aborted(slot);
    if (!pool_.slot(slot).connected_) {
        throw pool_.closing_error();
    }
}

template <typename io_type>
void db_pipeline_state<io_type>::send(member_type& member, statement_type& statement) {
    if (member.stopped()) {
        throw cancelled();
    }
    if (member.deadline_.has_value() && *member.deadline_ <= std::chrono::steady_clock::now()) {
        throw pool_.timed_out();
    }
    const auto index = member.slot_;
    auto& slot = pool_.slot(index);
    auto& sending = slot.session_.pipeline_;
    require_usable(index);
    typename io_type::flight_type flight{};
    const bool wake = io_type::queue(*this, index, member, statement, flight);
    auto& record = sending.record(sending.pending_);
    record = {&member, std::move(flight), member.rows_ != nullptr};
    ++sending.pending_;
    --sending.reserved_;
    member.record_ = &record;
    member.phase_ = db_pipeline_phase::pending;
    if (sending.reader_ == nullptr) {
        sending.reader_ = &member;
        return;
    }
    if (!slot.wait_active_) {
        return;
    }
    // The waiting reader looks again when the driver asks for it, or when
    // this statement's deadline comes first.
    const bool earlier = member.deadline_.has_value() &&
                         (!sending.wait_deadline_.has_value() || *member.deadline_ < *sending.wait_deadline_);
    if (wake || earlier) {
        slot.wait_socket_.cancel();
    }
}

template <typename io_type>
bool db_pipeline_state<io_type>::reads(const member_type& member) const noexcept {
    return group(member.slot_).reader_ == &member;
}

template <typename io_type>
bool db_pipeline_state<io_type>::retries(member_type& member, db_pipeline_event event) const {
    switch (event) {
        case db_pipeline_event::completed:
            if (member.failure_ != nullptr) {
                std::rethrow_exception(member.failure_);
            }
            return false;
        case db_pipeline_event::retry:
            return true;
        case db_pipeline_event::failed:
            std::rethrow_exception(member.failure_);
        case db_pipeline_event::timed_out:
            throw pool_.timed_out();
        case db_pipeline_event::cancelled:
            throw cancelled();
        default:
            std::terminate();
    }
}

template <typename io_type>
void db_pipeline_state<io_type>::complete_head(std::size_t slot, member_type& reader, db_pipeline_outcome outcome) {
    auto& completing = group(slot);
    auto& record = completing.records_[completing.head_];
    completing.stretch_start_.reset();
    auto* member = record.member_;
    record = {};
    completing.head_ = (completing.head_ + 1) % db_pipeline_depth;
    --completing.pending_;
    if (member == nullptr) {
        dispatch();
        return;
    }
    member->record_ = nullptr;
    auto event = db_pipeline_event::completed;
    if (outcome.retry_) {
        // The retry keeps the room it frees, and goes to the tail.
        member->may_retry_ = false;
        member->phase_ = db_pipeline_phase::reserved;
        ++completing.reserved_;
        event = db_pipeline_event::retry;
    } else {
        member->phase_ = db_pipeline_phase::settled;
        member->affected_rows_ = outcome.affected_rows_;
        member->insert_id_ = outcome.insert_id_;
        member->failure_ = std::move(outcome.failure_);
    }
    if (member == &reader) {
        member->event_ = event;
    } else {
        settle(*member, event);
    }
    if (event != db_pipeline_event::retry) {
        dispatch();
    }
}

template <typename io_type>
bool db_pipeline_state<io_type>::read_buffered(member_type& reader) {
    const auto slot = reader.slot_;
    require_usable(slot);
    while (reader.event_ == db_pipeline_event::none) {
        if (!io_type::advance(*this, slot, reader)) {
            return false;
        }
    }
    // Results already available reach their members before the role passes
    // on; a failure among them no longer concerns the reader's own outcome.
    auto& reading = group(slot);
    try {
        while (reading.pending_ != 0 && !reading.closing_ && io_type::advance(*this, slot, reader)) {
        }
    } catch (...) {
        fail_group(slot, std::current_exception());
    }
    hand_off(reader);
    return true;
}

template <typename io_type>
void db_pipeline_state<io_type>::expire_followers(
    member_type& reader, std::chrono::steady_clock::time_point now) noexcept {
    auto& reading = group(reader.slot_);
    for (std::size_t offset = 0; offset < reading.pending_; ++offset) {
        auto* member = reading.record(offset).member_;
        if (member != nullptr && member != &reader && member->event_ == db_pipeline_event::none &&
            member->deadline_.has_value() && *member->deadline_ <= now) {
            abandon(*member);
            settle(*member, db_pipeline_event::timed_out);
        }
    }
}

template <typename io_type>
std::optional<std::chrono::steady_clock::time_point> db_pipeline_state<io_type>::earliest_deadline(
    std::size_t slot) const noexcept {
    const auto& reading = group(slot);
    std::optional<std::chrono::steady_clock::time_point> earliest;
    for (std::size_t offset = 0; offset < reading.pending_; ++offset) {
        const auto* member = reading.record(offset).member_;
        if (member != nullptr && member->event_ == db_pipeline_event::none && member->deadline_.has_value() &&
            (!earliest.has_value() || *member->deadline_ < *earliest)) {
            earliest = member->deadline_;
        }
    }
    return earliest;
}

template <typename io_type>
void db_pipeline_state<io_type>::hand_off(member_type& reader) noexcept {
    auto& reading = group(reader.slot_);
    if (reading.reader_ != &reader) {
        return;
    }
    reading.reader_ = nullptr;
    for (std::size_t offset = 0; offset < reading.pending_; ++offset) {
        auto* member = reading.record(offset).member_;
        if (member != nullptr && member != &reader && member->event_ == db_pipeline_event::none) {
            reading.reader_ = member;
            settle(*member, db_pipeline_event::reader);
            return;
        }
    }
}

template <typename io_type>
void db_pipeline_state<io_type>::abandon(member_type& member) noexcept {
    if (member.record_ != nullptr) {
        member.record_->member_ = nullptr;
        member.record_ = nullptr;
    }
    member.phase_ = db_pipeline_phase::settled;
}

template <typename io_type>
void db_pipeline_state<io_type>::clear_flights(group_type& cleared) noexcept {
    for (std::size_t offset = 0; offset < cleared.pending_; ++offset) {
        cleared.record(offset) = {};
    }
    cleared.io_.reset();
    cleared.wait_deadline_.reset();
    cleared.stretch_start_.reset();
    cleared.reader_ = nullptr;
    cleared.head_ = 0;
    cleared.pending_ = 0;
}

// The first failure is the group's; a designated reader that has not resumed
// yet learns it instead of its role.
template <typename io_type>
void db_pipeline_state<io_type>::fail_group(std::size_t slot, const std::exception_ptr& failure) noexcept {
    auto& failing = group(slot);
    if (!failing.closing_) {
        failing.closing_ = true;
        failing.failure_ = failure;
    }
    for (std::size_t offset = 0; offset < failing.pending_; ++offset) {
        auto* member = failing.record(offset).member_;
        if (member == nullptr) {
            continue;
        }
        member->record_ = nullptr;
        member->phase_ = db_pipeline_phase::settled;
        if (member->event_ == db_pipeline_event::none || member->event_ == db_pipeline_event::reader) {
            member->failure_ = failing.failure_;
            settle(*member, db_pipeline_event::failed);
        }
    }
    clear_flights(failing);
    pool_.close_slot(slot);
}

template <typename io_type>
void db_pipeline_state<io_type>::unlink(member_type& member) noexcept {
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
template <typename io_type>
void db_pipeline_state<io_type>::cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
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
        case db_pipeline_phase::parked:
            unpark(*member);
            member->phase_ = db_pipeline_phase::admitting;
            settle(*member, db_pipeline_event::cancelled);
            return;
        case db_pipeline_phase::leased:
            if (member->connecting_) {
                pool_.close_slot(member->slot_);
            }
            return;
        case db_pipeline_phase::pending: {
            auto& slot = pool_.slot(member->slot_);
            if (slot.session_.pipeline_.reader_ == member) {
                // The reader leaves its wait and passes the role on.
                if (slot.wait_active_) {
                    slot.wait_socket_.cancel();
                }
            } else if (member->event_ == db_pipeline_event::none) {
                abandon(*member);
                settle(*member, db_pipeline_event::cancelled);
            }
            return;
        }
        // Running, or about to: it sees stopped() at its next step.
        case db_pipeline_phase::admitting:
        case db_pipeline_phase::reserved:
        case db_pipeline_phase::settled:
            return;
    }
}

template <typename io_type>
void db_pipeline_state<io_type>::retire(member_type& member) noexcept {
    --statements_;
    if (!member.cancel_requested_) {
        unlink(member);
    }
    switch (member.phase_) {
        case db_pipeline_phase::admitting:
            break;
        case db_pipeline_phase::parked:
            unpark(member);
            break;
        case db_pipeline_phase::leased:
            member.phase_ = db_pipeline_phase::admitting;
            pool_.release_slot(member.slot_);
            break;
        case db_pipeline_phase::reserved:
            --group(member.slot_).reserved_;
            leave(member);
            break;
        case db_pipeline_phase::pending:
            hand_off(member);
            abandon(member);
            leave(member);
            break;
        case db_pipeline_phase::settled:
            hand_off(member);
            leave(member);
            break;
    }
}

template <typename io_type>
void db_pipeline_state<io_type>::leave(member_type& member) noexcept {
    member.phase_ = db_pipeline_phase::admitting;
    if (--group(member.slot_).members_ != 0) {
        dispatch();
        return;
    }
    dissolve(member.slot_);
}

// The last member gives the connection back. A failed or incomplete protocol
// exchange closes it here; release_slot() checks transaction state before
// either scheduler hands it to another caller.
template <typename io_type>
void db_pipeline_state<io_type>::dissolve(std::size_t slot) noexcept {
    auto& connection_slot = pool_.slot(slot);
    auto& dissolving = connection_slot.session_.pipeline_;
    if (dissolving.closing_ || !connection_slot.connected_ || !io_type::reusable(*this, slot)) {
        pool_.close_slot(slot);
    }
    clear_flights(dissolving);
    dissolving.failure_ = nullptr;
    dissolving.reserved_ = 0;
    dissolving.closing_ = false;
    pool_.release_slot(slot);
}

// Reads the group's results for one statement while it holds the reader role,
// from taking the role to the statement's outcome. A stop or the statement's
// deadline abandons the statement, which passes the role on as it leaves; any
// other failure while reading fails the connection for every member.
template <typename io_type>
class db_pipeline_reader final {
public:
    using clock_type = std::chrono::steady_clock;
    using state_type = db_pipeline_state<io_type>;
    using pool_type = typename state_type::pool_type;
    using member_type = typename state_type::member_type;

    db_pipeline_reader(pool_type& pool, member_type& member) noexcept
        : pool_(pool),
          member_(member) {}

    db_pipeline_reader(const db_pipeline_reader&) = delete;
    db_pipeline_reader& operator=(const db_pipeline_reader&) = delete;

    // One socket wait; a failure of the wait itself fails the connection.
    // Without a socket wait it pauses for one worker turn instead: the driver
    // has queued nothing to wait for yet, and statements arriving in this
    // turn should go out with what it just sent.
    class socket_wait final {
    public:
        explicit socket_wait(db_pipeline_reader& reader) noexcept
            : reader_(reader) {}
        socket_wait(db_pipeline_reader& reader, std::size_t slot, db_slot_socket::native_socket_type native,
            db_socket_readiness wanted, std::optional<clock_type::time_point> deadline) noexcept
            : reader_(reader) {
            wait_.emplace(reader.pool_, slot, db_deadline_kind::socket, native, wanted, deadline, reader.unbounded_);
        }
        socket_wait(const socket_wait&) = delete;
        socket_wait& operator=(const socket_wait&) = delete;

        [[nodiscard]] bool await_ready() {
            if (!wait_.has_value()) {
                return false;
            }
            try {
                return wait_->await_ready();
            } catch (...) {
                reader_.fail(std::current_exception());
            }
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept {
            if (!wait_.has_value()) {
                reader_.pool_.state().worker().defer_resume(handle);
                return true;
            }
            return wait_->await_suspend(handle);
        }
        void await_resume() {
            if (!wait_.has_value()) {
                return;
            }
            try {
                reader_.waited(wait_->await_resume());
            } catch (...) {
                reader_.fail(std::current_exception());
            }
        }

    private:
        db_pipeline_reader& reader_;
        std::optional<typename pool_type::slot_wait> wait_;
    };

    // Reads what is available; true once the statement has its outcome.
    [[nodiscard]] bool read() {
        auto& state = pool_.state();
        if (member_.stopped()) {
            throw state.cancelled();
        }
        const auto now = clock_type::now();
        if (member_.deadline_.has_value() && *member_.deadline_ <= now) {
            throw pool_.timed_out();
        }
        state.expire_followers(member_, now);
        try {
            return state.read_buffered(member_);
        } catch (...) {
            fail(std::current_exception());
        }
    }

    // Waits for what the driver needs until the earliest deadline of a
    // pending member or the I/O timeout of the direction waited for, counted
    // from the connection's last progress.
    [[nodiscard]] socket_wait wait() {
        try {
            const auto slot = member_.slot_;
            auto& group = pool_.state().group(slot);
            const auto wanted = io_type::wanted(pool_, slot);
            if (wanted == db_socket_readiness::none) {
                return socket_wait(*this);
            }
            const auto now = clock_type::now();
            if (!group.stretch_start_.has_value()) {
                group.stretch_start_ = now;
            }
            io_deadline_.reset();
            if (const auto io_timeout = db_io_timeout(pool_.config(), wanted)) {
                io_deadline_ = saturating_deadline(
                    *group.stretch_start_, io_timeout->value());
            }
            auto until = pool_.state().earliest_deadline(slot);
            if (io_deadline_.has_value() && (!until.has_value() || *io_deadline_ < *until)) {
                until = io_deadline_;
            }
            group.wait_deadline_ = until;
            return socket_wait(*this, slot, io_type::socket(pool_, slot), wanted, until);
        } catch (...) {
            fail(std::current_exception());
        }
    }

private:
    // A cancelled wait (a stop, a member joining with an earlier deadline or
    // a driver asking to look again) returns to read(), which looks again.
    void waited(const db_socket_wait_result& result) {
        auto& state = pool_.state();
        auto& group = state.group(member_.slot_);
        group.wait_deadline_.reset();
        check_db_socket_wait(result, io_type::driver_type::driver, io_type::driver_type::name);
        if (result.ready_ != db_socket_readiness::none) {
            group.stretch_start_.reset();
            io_type::exchange(state, member_.slot_, result.ready_);
        } else if (result.expired_ && io_deadline_.has_value() && clock_type::now() >= *io_deadline_) {
            throw pool_.timed_out();
        }
    }

    [[noreturn]] void fail(const std::exception_ptr& failure) {
        auto& state = pool_.state();
        state.fail_group(member_.slot_, failure);
        if (member_.stopped()) {
            throw state.cancelled();
        }
        std::rethrow_exception(member_.failure_ != nullptr ? member_.failure_ : failure);
    }

    pool_type& pool_;
    member_type& member_;
    const operation_deadline unbounded_{std::nullopt};
    std::optional<clock_type::time_point> io_deadline_{};
};

// One buffered statement outside a transaction, as a member of a pipeline
// group: admission, a connection of its own when it founds a group, its
// flight, then its outcome -- read by itself while it holds the reader role,
// else handed over by the reader. Every wait is an awaiter on this frame, so a
// statement costs no coroutine frame beyond this one unless it connects. The
// member's destructor gives back whatever it holds on every exit path.
template <typename io_type, typename result_type>
task<result_type> run_pipelined(typename db_pipeline_state<io_type>::pool_type& pool, std::string sql,
    std::vector<db_value> params, operation_options options,
    request_io_metrics* io_metrics) {
    using member_type = db_pipeline_member<io_type>;
    constexpr bool returns_rows = std::is_same_v<result_type, db_rows>;
    scoped_request_io_timer timer(io_metrics, request_io_kind::db);
    auto statement = io_type::prepare(sql, std::span<const db_value>(params));
    const operation_deadline timeout(options.timeout_);
    auto& pipeline = pool.state();
    auto output = db_result_access::make_result();
    member_type member(pipeline, returns_rows ? &output : nullptr, options.stop_token_);

    // Waiting for admission is bounded like acquiring a lease.
    if (pipeline.admit(member, timeout.constrained_by(pool.config().acquire_timeout_)) == db_pipeline_event::none) {
        pipeline.admitted(co_await typename member_type::wait{member});
    }
    if (member.phase_ == db_pipeline_phase::leased) {
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
        pipeline.send(member, statement);
        auto event = db_pipeline_event::none;
        while (event == db_pipeline_event::none) {
            if (pipeline.reads(member)) {
                db_pipeline_reader<io_type> reader(pool, member);
                while (!reader.read()) {
                    co_await reader.wait();
                }
                event = member.take_event();
            } else if ((event = co_await typename member_type::wait{member}) == db_pipeline_event::reader) {
                event = db_pipeline_event::none;
            }
        }
        if (!pipeline.retries(member, event)) {
            break;
        }
    }
    if constexpr (returns_rows) {
        co_return output;
    } else {
        co_return db_result_access::make_exec_result(member.affected_rows_, member.insert_id_);
    }
}

}  // namespace ruvia::detail
