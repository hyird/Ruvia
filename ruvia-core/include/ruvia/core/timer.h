#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <utility>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia {

enum class timer_sleep_result : std::uint8_t {
    elapsed,
    // The sleep was cut short because a stop was requested: the worker is
    // shutting down, or -- when a stop_token was passed -- whatever else owns
    // that token asked the work to stop. Named for the request rather than
    // for one of its causes, because a caller reacts to it the same way either
    // way: give up the rest of the wait.
    stop_requested,
};

class sleep_operation;

// Suspend the current coroutine on its bound worker for `duration`. The result
// distinguishes a normal elapsed delay from cancellation caused by worker
// shutdown (stop_timers): a sleep pending at that point, or started after the
// worker's timer queue stopped, returns stop_requested without throwing.
// Durations saturate to the steady-clock range, so an effectively unbounded
// delay such as hours::max() waits for stop instead of overflowing.
//
// The sleep is also cut short when `stop_token_value` is stopped. A
// framework-provided wait that ignores the caller's stop token is a hole in
// every deadline built on that token, so anything a request can await takes
// one. The stop callback may run on a thread that is not this worker, and
// cancelling a worker timer is worker-owned state, so cancellation is deferred
// onto the worker and validated there -- a stop that arrives after the sleep
// already finished is a no-op rather than a use-after-free.
//
// The worker is borrowed for the lifetime of the returned operation: the
// caller must keep its address-stable handle alive until the operation
// completes or is dropped cold. This is the same borrow-only boundary the
// request hot path applies everywhere else; a temporary handle would dangle in
// the cold operation.
template <typename rep_type, typename period_type>
[[nodiscard]] sleep_operation sleep_for(const worker_handle& worker_value,
    std::chrono::duration<rep_type, period_type> duration, stop_token stop_token_value = {});
template <typename rep_type, typename period_type>
sleep_operation sleep_for(worker_handle&&, std::chrono::duration<rep_type, period_type>, stop_token = {}) = delete;

// One sleep on the caller's worker: single-use, movable only before it is
// awaited, and started on the borrowed worker by co_await. An empty stop token
// makes it a plain timer wait; a stoppable one also cancels the timer when stop
// is requested.
class sleep_operation final {
public:
    sleep_operation(const sleep_operation&) = delete;
    sleep_operation& operator=(const sleep_operation&) = delete;
    sleep_operation(sleep_operation&& other) noexcept;
    sleep_operation& operator=(sleep_operation&&) = delete;

    [[nodiscard]] bool await_ready();
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
    [[nodiscard]] timer_sleep_result await_resume() const noexcept;

private:
    template <typename rep_type, typename period_type>
    friend sleep_operation sleep_for(const worker_handle&, std::chrono::duration<rep_type, period_type>, stop_token);

    sleep_operation(const worker_handle& worker_value, std::chrono::steady_clock::duration duration,
        stop_token stop_token_value) noexcept
        : worker_(&worker_value),
          duration_(duration),
          stop_token_(std::move(stop_token_value)) {}

    const worker_handle* worker_;
    std::chrono::steady_clock::duration duration_;
    stop_token stop_token_;
    std::coroutine_handle<> continuation_{};
    worker_timer_outcome outcome_{worker_timer_outcome::expired};
    bool started_{false};
    worker_timer_registration registration_;
    // Declared last so callback teardown completes before the timer registration
    // and the borrowed worker begin destruction.
    stop_registration stop_registration_;
};

template <typename rep_type, typename period_type>
sleep_operation sleep_for(const worker_handle& worker_value,
    std::chrono::duration<rep_type, period_type> duration, stop_token stop_token_value) {
    return sleep_operation(worker_value, worker_timer_saturating_duration_cast(duration), std::move(stop_token_value));
}

}  // namespace ruvia
