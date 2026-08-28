#pragma once

#include <chrono>
#include <cstdint>

#include <ruvia/core/StopToken.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>

namespace ruvia {

enum class TimerSleepResult : std::uint8_t {
    kElapsed,
    // The sleep was cut short because a stop was requested: the worker is
    // shutting down, or -- for the overload taking a StopToken -- whatever else
    // owns that token asked the work to stop. Named for the request rather than
    // for one of its causes, because a caller reacts to it the same way either
    // way: give up the rest of the wait.
    kStopRequested,
};

// Suspend the current coroutine on its bound worker for `duration`. The result
// distinguishes a normal elapsed delay from cancellation caused by worker
// shutdown (stopTimers).
//
// The worker is borrowed for the lifetime of the returned Task: the caller
// must keep its address-stable handle alive until the Task completes or is
// cancelled and joined. This is the same borrow-only boundary the request hot
// path applies everywhere else; a temporary handle would dangle in the lazy
// coroutine frame.
[[nodiscard]] Task<TimerSleepResult> sleepFor(const WorkerHandle& worker, std::chrono::steady_clock::duration duration);
Task<TimerSleepResult> sleepFor(WorkerHandle&&, std::chrono::steady_clock::duration) = delete;

// The same sleep, cut short when `stopToken` is stopped as well as by worker
// shutdown. A framework-provided wait that ignores the caller's stop token is a
// hole in every deadline built on that token, so anything a request can await
// should take one.
//
// The stop callback may run on a thread that is not this worker, and cancelling
// a worker timer is worker-owned state, so cancellation is deferred onto the
// worker and validated there -- a stop that arrives after the sleep already
// finished is a no-op rather than a use-after-free.
[[nodiscard]] Task<TimerSleepResult> sleepFor(const WorkerHandle& worker, std::chrono::steady_clock::duration duration, StopToken stopToken);
Task<TimerSleepResult> sleepFor(WorkerHandle&&, std::chrono::steady_clock::duration, StopToken) = delete;

}  // namespace ruvia
