#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "operation_options.h"
#include "stop_token.h"
#include "suspend_race.h"
#include "wait_result.h"
#include "worker_ref.h"
#include "worker_timer.h"

namespace ruvia {

class pool_lease_acquire_operation;

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

    [[nodiscard]] pool_lease_acquire_operation acquire(operation_options options = {}) &;
    pool_lease_acquire_operation acquire(operation_options = {}) && = delete;

    void release(std::size_t slot) noexcept;
    [[nodiscard]] bool close() noexcept;
    [[nodiscard]] bool closed() const noexcept;

private:
    friend class pool_lease_acquire_operation;

    class impl_type;
    std::unique_ptr<impl_type> impl_;
};

// One acquire: single-use, movable only before it is awaited. The scheduler
// must outlive it; destroying a scheduler with live acquires terminates.
class pool_lease_acquire_operation final {
public:
    pool_lease_acquire_operation(pool_lease_acquire_operation&& other) noexcept;
    pool_lease_acquire_operation(const pool_lease_acquire_operation&) = delete;
    pool_lease_acquire_operation& operator=(const pool_lease_acquire_operation&) = delete;
    pool_lease_acquire_operation& operator=(pool_lease_acquire_operation&&) = delete;
    void operator&() const = delete;
    ~pool_lease_acquire_operation();

    [[nodiscard]] bool await_ready();
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
    [[nodiscard]] wait_result<std::size_t> await_resume();

private:
    friend class pool_lease_scheduler;

    pool_lease_acquire_operation(pool_lease_scheduler::impl_type& owner_value, operation_options options) noexcept;
    void cancel_registrations() noexcept;
    [[nodiscard]] std::coroutine_handle<> resolve(wait_result<std::size_t> result_value) noexcept;
    void on_end(wait_status status) noexcept;

    pool_lease_scheduler::impl_type* owner_;
    pool_lease_acquire_operation* previous_{nullptr};
    pool_lease_acquire_operation* next_{nullptr};
    wait_bounds bounds_;
    suspend_race_state<wait_result<std::size_t>> completion_;
    std::uint64_t waiter_id_{0};
    bool queued_{false};
    bool awaited_{false};
    // Last so callback unregistration completes before anything it touches
    // begins destruction.
    stop_registration stop_registration_;
};

}  // namespace ruvia
