#pragma once

#include <chrono>

#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>

namespace ruvia {

// Suspend the current coroutine on its bound worker for `duration`. Returns
// true if the full duration elapsed, false if the sleep was cancelled by worker
// shutdown (stopTimers). A periodic loop should check the result and stop on
// false: calling sleepFor again after the worker's timer queue is stopping
// throws std::runtime_error from the scheduler.
[[nodiscard]] Task<bool> sleepFor(WorkerHandle worker, std::chrono::steady_clock::duration duration);

}  // namespace ruvia
