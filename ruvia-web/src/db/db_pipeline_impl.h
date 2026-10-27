#pragma once

// Definitions of db_pipeline.h, included by the driver translation units that
// instantiate a pipeline, after db_pool.h.
//
// The io_type hooks, all static, each on a connected, leased slot of a group:
//   statement_type prepare(pool_type&, std::string_view sql, std::span<const db_value>)
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
//   void check_wait(const db_socket_wait_result&)
//       Throws on a wait that failed the socket.
//   void exchange(state_type&, std::size_t slot, db_socket_readiness ready)
//       Continues the protocol after a readiness wake.
//   bool reusable(state_type&, std::size_t slot) noexcept
//       Whether the connection of a dissolving group, with no member left, can
//       go back to the pool: nothing left on the wire, outside a transaction.
// group_data has reset(), called when its group's flights are cleared.

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/integration/request_io_metrics.h"

#include "db/db_pipeline.h"
#include "db/db_pool.h"

namespace ruvia::detail {

// db_config's read and write timeouts bound waiting without socket progress in
// that direction; waiting while output is pending waits to write.
[[nodiscard]] inline std::optional<std::chrono::milliseconds> db_io_timeout(
    const db_config_storage& config, db_socket_readiness wanted) noexcept {
    return has_db_socket_readiness(wanted, db_socket_readiness::write) ? config.write_timeout_ : config.read_timeout_;
}

template <typename io_type>
db_pipeline_member<io_type>::db_pipeline_member(
    state_type& state, db_rows* rows, std::pmr::memory_resource* resource, const stop_token& stop_token_value)
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
    ++state_.statements_;
}

template <typename io_type>
db_pipeline_member<io_type>::~db_pipeline_member() {
    state_.retire(*this);
}

template <typename io_type>
db_pipeline_state<io_type>::db_pipeline_state(pool_type& pool, const worker_handle& worker_value)
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
db_pipeline_event db_pipeline_state<io_type>::admit(member_type& member, const operation_timeout& admission_deadline) {
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
void db_pipeline_state<io_type>::park(member_type& member, const operation_timeout& admission_deadline) {
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

template <typename io_type>
bool db_pipeline_state<io_type>::must_connect(member_type& member) {
    pool_.reset_abort(member.slot_);
    if (member.stopped()) {
        throw cancelled();
    }
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

// The last member gives the connection back. A connection that failed, or
// that the driver cannot reuse -- results nobody reads are still on their way,
// or a statement left it inside a transaction block -- is closed.
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
                io_deadline_ = worker_timer_saturating_deadline(
                    *group.stretch_start_, worker_timer_saturating_duration_cast(*io_timeout));
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
        io_type::check_wait(result);
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
    const operation_timeout unbounded_{std::nullopt};
    std::optional<clock_type::time_point> io_deadline_{};
};

// One buffered statement outside a transaction, as a member of a pipeline
// group: admission, a connection of its own when it founds a group, its
// flight, then its outcome -- read by itself while it holds the reader role,
// else handed over by the reader. Every wait is an awaiter on this frame, so a
// statement costs no coroutine frame beyond this one unless it connects. The
// member's destructor gives back whatever it holds on every exit path.
template <typename io_type, typename result_type>
task<result_type> run_pipelined(typename db_pipeline_state<io_type>::pool_type& pool, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource, operation_options options,
    operation_scope* scope) {
    using member_type = db_pipeline_member<io_type>;
    constexpr bool returns_rows = std::is_same_v<result_type, db_rows>;
    scoped_request_io_timer timer(scope, request_io_kind::db);
    auto statement = io_type::prepare(pool, sql, std::span<const db_value>(params));
    const operation_timeout timeout(options.timeout_);
    auto& pipeline = pool.state();
    auto output = db_result_access::make_result(resource);
    member_type member(pipeline, returns_rows ? &output : nullptr, resource, options.stop_token_);

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
