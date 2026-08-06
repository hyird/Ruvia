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
// shutdown (stopTimers). This public entry owns a WorkerHandle copy so a Task
// may safely outlive the caller's handle variable. Worker-affine framework
// code should use detail::sleepForBorrowed() instead, because its stable owner
// already keeps the handle alive and the request hot path must not copy its
// shared dispatcher endpoint.
[[nodiscard]] Task<TimerSleepResult> sleepFor(WorkerHandle worker, std::chrono::steady_clock::duration duration);

namespace detail {

// Borrow the caller's address-stable worker handle for the lifetime of the
// returned Task. The caller must keep it alive until the Task completes or is
// cancelled and joined. This is deliberately an lvalue-only boundary: a
// temporary handle would dangle in the lazy coroutine frame.
[[nodiscard]] Task<TimerSleepResult> sleepForBorrowed(const WorkerHandle& worker, std::chrono::steady_clock::duration duration);
Task<TimerSleepResult> sleepForBorrowed(WorkerHandle&&, std::chrono::steady_clock::duration) = delete;

}  // namespace detail

}  // namespace ruvia
