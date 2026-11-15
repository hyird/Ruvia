#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "operation_options.h"
#include "stop_token.h"
#include "suspend_race.h"
#include "wait_result.h"
#include "waiter_list.h"
#include "worker_ref.h"
#include "worker_timer.h"

namespace ruvia {

class pool_lease_acquire;

// Single-worker owner of pool slot leases and their asynchronous wait queue.
// The worker endpoint is retained once by the scheduler. Acquires copy
// their inputs before return and borrow only their scheduler, which must
// outlive them.
class pool_lease_scheduler final {
public:
    pool_lease_scheduler(std::size_t pool_size, const worker_ref& worker_value);

    pool_lease_scheduler(const pool_lease_scheduler&) = delete;
    pool_lease_scheduler& operator=(const pool_lease_scheduler&) = delete;
    pool_lease_scheduler(pool_lease_scheduler&&) = delete;
    pool_lease_scheduler& operator=(pool_lease_scheduler&&) = delete;
    ~pool_lease_scheduler();

    // The returned awaitable is cold: nothing is checked or queued until it is
    // awaited, which must happen on the scheduler's worker. A free slot is
    // leased synchronously without suspending; only a saturated pool waits.
    // The result carries the leased slot index as its value, or closed,
    // worker_stopping, timed_out or cancelled. A timed-out waiter leaves other
    // queued acquisitions unchanged. A relative options.timeout_ bounds the
    // wait from await; a deadline that has passed times out a saturated
    // acquire at once. Once the worker's shutdown notification arrives or its
    // timer queue stops, queued acquires and saturated new ones complete with
    // worker_stopping, as channel and one_shot waits do; a free slot is still
    // leased. It never throws for that.
    [[nodiscard]] pool_lease_acquire acquire(operation_options options = {}) &;
    pool_lease_acquire acquire(operation_options = {}) && = delete;

    // Return a caller-owned slot on the bound worker. A queued acquire takes
    // ownership before this call returns, but its coroutine resumes in a later
    // worker turn, never inline here. Later cancellation or close() cannot
    // replace that committed acquisition.
    void release(std::size_t slot) noexcept;
    // Refuses new acquires and completes every queued acquire with closed;
    // returns false when already closing. Like release(), the queued coroutines
    // resume in a later worker turn, never inline here, so the caller finishes
    // its own teardown before any waiter observes the closure.
    [[nodiscard]] bool close() noexcept;
    [[nodiscard]] bool closing() const noexcept;

private:
    friend class pool_lease_acquire;

    class impl_type;
    std::unique_ptr<impl_type> impl_;
};

// One acquire: single-use, movable only before it is awaited. The scheduler
// must outlive it; destroying a scheduler with live acquires terminates.
class pool_lease_acquire final : public detail::waiter_link<pool_lease_acquire> {
public:
    pool_lease_acquire(pool_lease_acquire&& other) noexcept;
    pool_lease_acquire(const pool_lease_acquire&) = delete;
    pool_lease_acquire& operator=(const pool_lease_acquire&) = delete;
    pool_lease_acquire& operator=(pool_lease_acquire&&) = delete;
    void operator&() const = delete;
    ~pool_lease_acquire();

    [[nodiscard]] bool await_ready();
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
    [[nodiscard]] wait_result<std::size_t> await_resume();

private:
    friend class pool_lease_scheduler;

    pool_lease_acquire(pool_lease_scheduler::impl_type& owner_value, operation_options options) noexcept;
    void cancel_registrations() noexcept;
    // Commits the outcome; returns the continuation to resume, or null while
    // the acquire has not suspended yet.
    [[nodiscard]] std::coroutine_handle<> resolve(wait_result<std::size_t> result_value) noexcept;
    void on_end(wait_status status) noexcept;

    pool_lease_scheduler::impl_type* owner_;
    wait_bounds bounds_;
    detail::suspend_race_state<wait_result<std::size_t>> completion_;
    std::uint64_t waiter_id_{0};
    bool awaited_{false};
    // Last so callback unregistration completes before anything it touches
    // begins destruction.
    stop_registration stop_registration_;
};

}  // namespace ruvia
