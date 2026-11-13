#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait.h"

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
class pool_lease_acquire final {
public:
    pool_lease_acquire(pool_lease_acquire&& other) noexcept;
    pool_lease_acquire(const pool_lease_acquire&) = delete;
    pool_lease_acquire& operator=(const pool_lease_acquire&) = delete;
    pool_lease_acquire& operator=(pool_lease_acquire&&) = delete;
    void operator&() const = delete;
    ~pool_lease_acquire();

    [[nodiscard]] bool await_ready();
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
    [[nodiscard]] worker_wait_result<std::size_t> await_resume();

private:
    friend class pool_lease_scheduler;

    pool_lease_acquire(pool_lease_scheduler::impl_type& owner_value, std::optional<operation_timeout> timeout,
        stop_token stop_token_value) noexcept;
    void cancel_registrations() noexcept;
    void complete(worker_wait_result<std::size_t> result_value) noexcept;
    [[nodiscard]] std::coroutine_handle<> resolve(worker_wait_result<std::size_t> result_value) noexcept;
    [[nodiscard]] std::coroutine_handle<> resolve(worker_wait_status status) noexcept;

    pool_lease_scheduler::impl_type* owner_;
    std::optional<operation_timeout> timeout_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    stop_token stop_token_;
    std::optional<worker_wait_result<std::size_t>> result_;
    std::uint64_t waiter_id_{0};
    std::coroutine_handle<> continuation_{};
    pool_lease_acquire* previous_{nullptr};
    pool_lease_acquire* next_{nullptr};
    worker_timer_registration deadline_timer_;
    stop_registration stop_registration_;
    bool queued_{false};
    bool awaited_{false};
};

}  // namespace ruvia
