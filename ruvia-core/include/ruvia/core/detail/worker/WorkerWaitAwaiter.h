#pragma once

#include <chrono>
#include <coroutine>
#include <exception>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include <ruvia/core/WorkerHandle.h>
#include <ruvia/core/WorkerWaitResult.h>
#include <ruvia/core/detail/SuspendRaceState.h>
#include <ruvia/core/detail/worker/WorkerTimer.h>

namespace ruvia::detail {

// The worker wait completion race is the SuspendRaceState pattern with a
// WorkerWaitResult payload.
template <typename T>
using WorkerWaitAwaitState = SuspendRaceState<WorkerWaitResult<T>>;

// The suspend half both worker waits share: publish the continuation under the
// state's mutex, then arm the optional timeout. On expiry the waiter detaches
// itself from the state and completes as timed out, so a later completion finds
// no waiter; if arming throws, the waiter is detached before the throw
// propagates rather than being left published against a timer that never runs.
//
// `State` must expose `mutex`, `worker` and a `waiter` pointer comparable to
// `self` -- the contract ChannelState and OneShotState both satisfy.
template <typename T, typename State, typename Waiter>
[[nodiscard]] bool suspendWorkerWait(State& state, Waiter* self, WorkerWaitAwaitState<T>& completion, WorkerTimerRegistration& timer, const std::optional<std::chrono::steady_clock::duration>& timeout, std::coroutine_handle<> handle) {
    std::lock_guard lock(state.mutex);
    if (!completion.suspend(handle)) {
        return false;
    }
    if (timeout) {
        try {
            WorkerHandleAccess::scheduleTimer(state.worker, timer, workerTimerDeadlineAfter(*timeout), [&state, self, &completion](WorkerTimerOutcome outcome) {
                if (outcome == WorkerTimerOutcome::kExpired) {
                    std::lock_guard stateLock(state.mutex);
                    if (state.waiter == self) {
                        state.waiter = nullptr;
                        (void)completion.complete(WorkerWaitResultAccess::timedOut<T>());
                    }
                }
                completion.continuation().resume();
            });
        } catch (...) {
            if (state.waiter == self) {
                state.waiter = nullptr;
            }
            throw;
        }
    }
    return true;
}

}  // namespace ruvia::detail
