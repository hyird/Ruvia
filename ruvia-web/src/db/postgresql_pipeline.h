#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"

#include "db/db_postgresql.h"

// Pipelining of PostgreSQL buffered statements. A statement outside a
// transaction joins a pipeline group: one leased connection that several
// statements share in libpq pipeline mode, each sending its own flight ending
// in its own Sync, so a failed statement affects no other. Results are read in
// send order by one member at a time, the reader, which hands each finished
// statement's outcome to its member and passes the role on when its own
// statement completes. Members wait in their own coroutine frames; the group's
// bookkeeping is owned by the connection's slot and the pool, because a member
// that is cancelled leaves before its results arrive.

namespace ruvia::detail {

template <typename driver_type>
class db_pool;
struct postgresql_driver;
class postgresql_pipeline_state;
struct postgresql_pipeline_record;

// Statements one connection keeps in flight at once. Locally, 16 reached about
// six times the throughput of one statement per round trip, 8 somewhat less
// and 32 no more; a deeper pipeline only adds queueing latency.
inline constexpr std::size_t postgresql_pipeline_depth = 16;

// The commands of one buffered statement flight. A first use runs as the
// unnamed statement. Otherwise a Close of a statement evicted to make room and
// a Close and Parse of a statement new to the connection precede the
// statement. A Sync ends the flight.
struct postgresql_flight final {
    // The cache entry of a named statement.
    std::optional<postgresql_statement_cache::handle> statement_{};
    // Commands queued ahead of the statement.
    std::size_t setup_commands_{0};
    bool parses_{false};
};

// What has been read of one flight's results, up to its Sync.
struct postgresql_flight_results final {
    // Index of the command whose results are being read; each command's
    // results end with a null result, and the Sync result follows the last.
    std::size_t command_{0};
    std::uint64_t affected_rows_{0};
    // The first error of the flight; the server skips the rest up to Sync.
    std::optional<db_error> failure_{};
    // Keeping the rows failed (an allocation in the caller's resource); the
    // flight is still read up to its Sync, so only its statement fails.
    std::exception_ptr output_failure_{};
    // The error is the statement's own rather than its setup's.
    bool statement_failed_{false};
    // The error reports a stale cached statement (26000 / 0A000) ...
    bool stale_{false};
    // ... found while looking it up or revalidating it, before it ran.
    bool stale_before_execution_{false};
    bool returned_rows_{false};
    bool parsed_{false};
};

enum class postgresql_member_phase : std::uint8_t {
    // Holds nothing.
    admitting,
    // Waits in the pool's arrival order for room in a group or a connection.
    parked,
    // Holds a lease and founds a group on it.
    leased,
    // A member holding room for a flight it has not sent yet.
    reserved,
    // A member whose flight is in flight.
    pending,
    // A member whose statement has an outcome, or was abandoned.
    settled,
};

// What woke a waiting member.
enum class postgresql_member_event : std::uint8_t {
    none,
    // Admission: a connection was leased for it, or room in a group reserved.
    leased,
    joined,
    // It reads the group's results now.
    reader,
    // Its statement finished; failure_ tells how.
    completed,
    // Its cached statement was stale; it sends the statement once more.
    retry,
    // Its group's connection failed with failure_.
    failed,
    timed_out,
    cancelled,
    closing,
};

// One pipelined statement. It lives in its caller's coroutine frame; the
// group refers to it only while it is a member, through records that forget
// it when it leaves. Construction binds its stop token and makes it findable
// for cancellation; destruction gives back whatever it still holds -- a parked
// position, a lease, room or a flight in a group -- so every exit path of the
// statement shares one cleanup.
class postgresql_pipeline_member final {
public:
    postgresql_pipeline_member(postgresql_pipeline_state& state, db_rows* rows, std::pmr::memory_resource* resource,
        const stop_token& stop_token_value);
    ~postgresql_pipeline_member();

    postgresql_pipeline_member(const postgresql_pipeline_member&) = delete;
    postgresql_pipeline_member& operator=(const postgresql_pipeline_member&) = delete;

    // Suspends until an event arrives, unless one already has.
    struct wait final {
        postgresql_pipeline_member& member_;

        [[nodiscard]] bool await_ready() const noexcept {
            return member_.event_ != postgresql_member_event::none;
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept {
            member_.continuation_ = handle;
        }
        [[nodiscard]] postgresql_member_event await_resume() noexcept {
            return member_.take_event();
        }
    };

    [[nodiscard]] postgresql_member_event take_event() noexcept {
        return std::exchange(event_, postgresql_member_event::none);
    }
    // Its stop token was triggered, even if the cancellation has not been
    // dispatched yet.
    [[nodiscard]] bool stopped() const noexcept {
        return cancel_requested_ || stop_token_.stop_requested();
    }

    postgresql_pipeline_state& state_;
    // The caller's result for query(); null for execute().
    db_rows* rows_;
    std::pmr::memory_resource* resource_;
    // The caller's options outlive the statement's frame.
    const stop_token& stop_token_;
    std::uint64_t affected_rows_{0};
    std::exception_ptr failure_{};
    // The statement deadline, from its first send.
    std::optional<std::chrono::steady_clock::time_point> deadline_{};
    std::coroutine_handle<> continuation_{};
    // The record of its flight while pending.
    postgresql_pipeline_record* record_{nullptr};
    // Its place in the pool's arrival order, shared with transactions and
    // streams waiting for a lease.
    std::uint64_t arrival_{0};
    std::size_t slot_{0};
    postgresql_member_phase phase_{postgresql_member_phase::admitting};
    postgresql_member_event event_{postgresql_member_event::none};
    // Set when its cancellation is dispatched, which also unlinks it from the
    // statements a cancellation searches.
    bool cancel_requested_{false};
    bool connecting_{false};
    bool may_retry_{true};
    // The pool's FIFO of parked statements.
    postgresql_pipeline_member* previous_parked_{nullptr};
    postgresql_pipeline_member* next_parked_{nullptr};
    // The pool's cancellable statements, newest first.
    postgresql_pipeline_member* previous_{nullptr};
    postgresql_pipeline_member* next_{nullptr};
    worker_timer_registration admission_timer_;
    std::uint64_t cancellation_id_{0};
    worker_cancellation_registration<worker_cancellation_target<postgresql_pipeline_state>> cancellation_;
};

// One flight in a group, in send order.
struct postgresql_pipeline_record final {
    // Null once the member abandoned the statement; its results are discarded.
    postgresql_pipeline_member* member_{nullptr};
    postgresql_flight flight_{};
    bool returns_rows_{false};
};

// The pipeline group of one connection; it exists while it has members. The
// records form a ring of the flights in send order; the head is the one whose
// results are read next.
struct postgresql_pipeline_group final {
    explicit postgresql_pipeline_group(std::pmr::memory_resource* resource)
        : records_(postgresql_pipeline_depth, resource) {}

    // Its oldest flight was abandoned: a statement sent now would wait behind
    // one whose caller already gave up on it, so the group takes nobody new
    // until that flight's results have been read.
    [[nodiscard]] bool stalled() const noexcept {
        return pending_ != 0 && records_[head_].member_ == nullptr;
    }

    std::pmr::vector<postgresql_pipeline_record> records_;
    postgresql_flight_results head_results_{};
    // Why the connection failed, once closing_.
    std::exception_ptr failure_{};
    // When the reader's current socket wait ends at the latest.
    std::optional<std::chrono::steady_clock::time_point> wait_deadline_{};
    // Start of the connection's current stretch without progress -- no socket
    // readiness and no flight completed -- while flights are outstanding. The
    // I/O timeout counts from it whichever member holds the reader role.
    std::optional<std::chrono::steady_clock::time_point> stretch_start_{};
    postgresql_pipeline_member* reader_{nullptr};
    std::size_t head_{0};
    std::size_t pending_{0};
    // Room promised to members that have not sent their flight yet.
    std::size_t reserved_{0};
    std::size_t members_{0};
    // The connection failed; the group takes no members and dissolves.
    bool closing_{false};
    // libpq holds output the socket has not accepted yet.
    bool flush_pending_{false};
};

// Per connection: cached statements and the pipeline group sharing it.
struct postgresql_session final {
    explicit postgresql_session(std::pmr::memory_resource* resource)
        : statements_(resource),
          pipeline_(resource) {}

    // Cleared by connect(): a new connection holds no server statements.
    postgresql_statement_cache statements_;
    postgresql_pipeline_group pipeline_;
};

// Per pool: admission into groups, in arrival order with the transactions and
// streams waiting for a lease. A statement founds a group on a free
// connection, else joins the group with the most room, else parks in a FIFO
// served by freed room and by released connections. A statement never joins a
// group ahead of an older transaction or stream still waiting: the groups
// drain to it, and a released connection goes to it. A parked statement older
// than every waiting transaction and stream takes a released connection
// itself, before the lease scheduler can hand it on.
class postgresql_pipeline_state final {
public:
    using pool_type = db_pool<postgresql_driver>;
    using cancellation_target_type = worker_cancellation_target<postgresql_pipeline_state>;

    postgresql_pipeline_state(pool_type& pool, const worker_handle& worker_value);
    ~postgresql_pipeline_state();

    postgresql_pipeline_state(const postgresql_pipeline_state&) = delete;
    postgresql_pipeline_state& operator=(const postgresql_pipeline_state&) = delete;

    // db_pool hooks.
    [[nodiscard]] bool take_released_slot(std::size_t slot) noexcept;
    void exclusive_waiter_left() noexcept;
    // Fails every parked statement with closing.
    void close() noexcept;
    // worker_cancellation_target hook.
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept;

    [[nodiscard]] const std::shared_ptr<cancellation_target_type>& cancellation_target() const noexcept {
        return cancellation_target_;
    }

    // The statement's way through a group, in order. admit() places it, or
    // parks it to be woken with the event admitted() checks. A statement that
    // leased a connection connects it if needed and founds a group on it.
    [[nodiscard]] postgresql_member_event admit(
        postgresql_pipeline_member& member, const operation_timeout& admission_deadline);
    void admitted(postgresql_member_event event) const;
    [[nodiscard]] bool must_connect(postgresql_pipeline_member& member);
    void connected(postgresql_pipeline_member& member, const std::exception_ptr& failure);
    void found_group(postgresql_pipeline_member& member);
    // Sends the member's flight from its reserved room; it becomes the reader
    // when the group has none.
    void send(postgresql_pipeline_member& member, std::string_view sql, const postgresql_params& encoded);
    [[nodiscard]] bool reads(const postgresql_pipeline_member& member) const noexcept;
    // Turns the event that ended the member's wait into its outcome: false
    // once it completed, true when it must be sent once more; throws its
    // failure.
    [[nodiscard]] bool retries(postgresql_pipeline_member& member, postgresql_member_event event) const;

    // Services for the reader. read_buffered() reads the results libpq holds
    // and returns true once the reader's own statement finished, after
    // handing the role on.
    [[nodiscard]] bool read_buffered(postgresql_pipeline_member& reader);
    // Abandons the pending followers whose deadline passed.
    void expire_followers(postgresql_pipeline_member& reader, std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> earliest_deadline(std::size_t slot) const noexcept;
    // Fails every pending member and closes the connection.
    void fail_group(std::size_t slot, const std::exception_ptr& failure) noexcept;

private:
    friend class postgresql_pipeline_member;

    [[nodiscard]] postgresql_pipeline_group& group(std::size_t slot) const noexcept;
    // Whether a statement of this arrival may take a connection or room: no
    // older transaction or stream still waits for a lease.
    [[nodiscard]] bool may_share(std::uint64_t arrival) const noexcept;
    // Places a statement whose turn it is: a free connection, else room in the
    // group with the most of it that is not stalled.
    [[nodiscard]] postgresql_member_event place(postgresql_pipeline_member& member);
    [[nodiscard]] std::optional<std::size_t> roomiest_group() const noexcept;
    void join(postgresql_pipeline_member& member, std::size_t slot) noexcept;
    void park(postgresql_pipeline_member& member, const operation_timeout& admission_deadline);
    void unpark(postgresql_pipeline_member& member) noexcept;
    // Serves parked statements in arrival order.
    void dispatch() noexcept;
    // Records what woke a member and posts its resumption.
    void settle(postgresql_pipeline_member& member, postgresql_member_event event) noexcept;
    void require_usable(std::size_t slot) const;
    void read_result(std::size_t slot, postgresql_pipeline_member& reader);
    void complete_head(std::size_t slot, postgresql_pipeline_member& reader);
    void hand_off(postgresql_pipeline_member& reader) noexcept;
    void abandon(postgresql_pipeline_member& member) noexcept;
    void clear_flights(postgresql_pipeline_group& cleared) noexcept;
    void unlink(postgresql_pipeline_member& member) noexcept;
    void retire(postgresql_pipeline_member& member) noexcept;
    void leave(postgresql_pipeline_member& member) noexcept;
    void dissolve(std::size_t slot) noexcept;

    pool_type& pool_;
    const worker_handle& worker_;
    postgresql_pipeline_member* members_{nullptr};
    postgresql_pipeline_member* parked_head_{nullptr};
    postgresql_pipeline_member* parked_tail_{nullptr};
    std::shared_ptr<cancellation_target_type> cancellation_target_;
};

}  // namespace ruvia::detail
