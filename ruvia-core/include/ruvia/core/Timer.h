#pragma once

#include <chrono>
#include <cstdint>

#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>

namespace ruvia {

enum class TimerSleepResult : std::uint8_t {
    kElapsed,
    kWorkerStopping,
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

}  // namespace ruvia
