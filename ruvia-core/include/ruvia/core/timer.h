#pragma once

#include <chrono>
#include <cstdint>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia {

enum class timer_sleep_result : std::uint8_t {
    elapsed,
    // The sleep was cut short because a stop was requested: the worker is
    // shutting down, or -- for the overload taking a stop_token -- whatever else
    // owns that token asked the work to stop. Named for the request rather than
    // for one of its causes, because a caller reacts to it the same way either
    // way: give up the rest of the wait.
    stop_requested,
};

// Suspend the current coroutine on its bound worker for `duration`. The result
// distinguishes a normal elapsed delay from cancellation caused by worker
// shutdown (stop_timers): a sleep pending at that point, or started after the
// worker's timer queue stopped, returns stop_requested without throwing.
//
// The worker is borrowed for the lifetime of the returned task: the caller
// must keep its address-stable handle alive until the task completes or is
// cancelled and joined. This is the same borrow-only boundary the request hot
// path applies everywhere else; a temporary handle would dangle in the lazy
// coroutine frame.
[[nodiscard]] task<timer_sleep_result> sleep_for(
    const worker_handle& worker_value, std::chrono::steady_clock::duration duration);
task<timer_sleep_result> sleep_for(worker_handle&&, std::chrono::steady_clock::duration) = delete;

// The same sleep, cut short when `get_stop_token` is stopped as well as by worker
// shutdown. A framework-provided wait that ignores the caller's stop token is a
// hole in every deadline built on that token, so anything a request can await
// should take one.
//
// The stop callback may run on a thread that is not this worker, and cancelling
// a worker timer is worker-owned state, so cancellation is deferred onto the
// worker and validated there -- a stop that arrives after the sleep already
// finished is a no-op rather than a use-after-free.
[[nodiscard]] task<timer_sleep_result> sleep_for(
    const worker_handle& worker_value, std::chrono::steady_clock::duration duration, stop_token stop_token_value);
task<timer_sleep_result> sleep_for(
    worker_handle&&, std::chrono::steady_clock::duration, stop_token) = delete;

}  // namespace ruvia
