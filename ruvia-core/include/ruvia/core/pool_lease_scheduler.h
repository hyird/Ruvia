#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait_result.h"

namespace ruvia {
namespace detail {
class pool_waiter_queue;
}  // namespace detail

class pool_lease_acquire;

// Single-worker owner of pool slot leases and their asynchronous wait queue.
// The worker endpoint is retained once by the scheduler. Acquires copy
// their inputs before return and borrow only their scheduler, which must
// outlive them.
class pool_lease_scheduler final {
public:
    pool_lease_scheduler(std::size_t pool_size, const worker_handle& worker_value,
        std::pmr::memory_resource* resource = nullptr);

    pool_lease_scheduler(const pool_lease_scheduler&) = delete;
    pool_lease_scheduler& operator=(const pool_lease_scheduler&) = delete;
    pool_lease_scheduler(pool_lease_scheduler&&) = delete;
    pool_lease_scheduler& operator=(pool_lease_scheduler&&) = delete;
    ~pool_lease_scheduler();

    // The returned awaitable is cold: nothing is checked or queued until it is
    // awaited, which must happen on the scheduler's worker. A free slot is
    // leased synchronously without suspending; only a saturated pool waits.
    // The result carries the leased slot index as its value, or closed,
    // timed_out or cancelled; it is never worker_stopping.
    // A timed-out waiter leaves other queued acquisitions unchanged.
    // A timeout arms a timer on the scheduler's worker. Once that
    // worker's timer queue stops (pending or new acquires alike) no timer
    // fires: the acquire completes only through release(), close() (closed),
    // or its stop token (cancelled). It never throws for that.
    [[nodiscard]] pool_lease_acquire acquire(const std::optional<std::chrono::milliseconds>& timeout);
    [[nodiscard]] pool_lease_acquire acquire(const std::optional<std::chrono::milliseconds>& timeout,
        stop_token stop_token);

    void release(std::size_t slot) noexcept;
    [[nodiscard]] bool close() noexcept;
    [[nodiscard]] bool closing() const noexcept;

private:
    friend class pool_lease_acquire;

    class impl_type;
    std::unique_ptr<impl_type, pmr_object_deleter<impl_type>> impl_;
};

// One acquire: single-use, movable only before it is awaited. The scheduler
// must outlive it; destroying a scheduler with live acquires terminates.
class pool_lease_acquire final {
public:
    pool_lease_acquire(pool_lease_acquire&& other) noexcept;
    pool_lease_acquire(const pool_lease_acquire&) = delete;
    pool_lease_acquire& operator=(const pool_lease_acquire&) = delete;
    pool_lease_acquire& operator=(pool_lease_acquire&&) = delete;
    ~pool_lease_acquire();

    [[nodiscard]] bool await_ready();
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
    [[nodiscard]] worker_wait_result<std::size_t> await_resume();

private:
    friend class pool_lease_scheduler;
    friend class detail::pool_waiter_queue;

    pool_lease_acquire(pool_lease_scheduler::impl_type& owner_value,
        std::optional<std::chrono::milliseconds> timeout, stop_token stop_token_value) noexcept;
    void cancel_registrations() noexcept;
    void complete(worker_wait_result<std::size_t> result_value) noexcept;
    void complete(worker_wait_status status) noexcept;
    void resume() noexcept;
    [[nodiscard]] std::coroutine_handle<> take_continuation() noexcept;

    pool_lease_scheduler::impl_type* owner_;
    std::optional<std::chrono::milliseconds> timeout_;
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
