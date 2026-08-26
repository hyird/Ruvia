#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include <ruvia/core/StopToken.h>
#include <ruvia/core/WorkerHandle.h>
#include <ruvia/core/WorkerWaitResult.h>
#include <ruvia/core/detail/SuspendRaceState.h>
#include <ruvia/core/detail/worker/WorkerTimer.h>

namespace ruvia::detail {

// The worker wait completion race is the SuspendRaceState pattern with a
// WorkerWaitResult payload.
template <typename T>
using WorkerWaitAwaitState = SuspendRaceState<WorkerWaitResult<T>>;

template <typename State>
void completeWorkerSingleWait(State& state, WorkerWaitStatus status) noexcept;

template <typename State>
void cancelWorkerSingleWait(const std::shared_ptr<State>& state, std::uint64_t generation) noexcept;

// Shared storage and transition machinery for one worker-bound waiter. Awaiter
// remains a template parameter, so publishing, cancellation, and wake-up use a
// direct typed pointer with no allocation, virtual dispatch, or type erasure.
// State must expose worker, mutex, waiter, waiterGeneration, and
// nextWaiterGeneration.
template <typename T, typename State, typename Awaiter>
class WorkerSingleWaitAwaiter {
public:
    WorkerSingleWaitAwaiter(std::shared_ptr<State> state,
        std::optional<std::chrono::steady_clock::duration> timeout, StopToken stopToken)
        : state_(std::move(state)),
          timeout_(timeout),
          stopToken_(std::move(stopToken)),
          generation_(stopToken_.stoppable() ? reserveGeneration(*state_) : 0),
          stopRegistration_(registerCancellation(stopToken_, state_, generation_)) {}

    WorkerSingleWaitAwaiter(const WorkerSingleWaitAwaiter&) = delete;
    WorkerSingleWaitAwaiter& operator=(const WorkerSingleWaitAwaiter&) = delete;
    WorkerSingleWaitAwaiter(WorkerSingleWaitAwaiter&&) = delete;
    WorkerSingleWaitAwaiter& operator=(WorkerSingleWaitAwaiter&&) = delete;

    [[nodiscard]] State& state() const noexcept {
        return *state_;
    }

    [[nodiscard]] const StopToken& stopToken() const noexcept {
        return stopToken_;
    }

    [[nodiscard]] const std::optional<std::chrono::steady_clock::duration>& timeout()
        const noexcept {
        return timeout_;
    }

    [[nodiscard]] bool completeResult(WorkerWaitResult<T>&& result) {
        return completion_.complete(std::move(result));
    }

    [[nodiscard]] bool completeStatus(WorkerWaitStatus status) noexcept {
        return completion_.complete(WorkerWaitResultAccess::outcome<T>(status));
    }

    void publish() noexcept {
        auto& owner = state();
        owner.waiter = self();
        owner.waiterGeneration = generation_;
    }

    [[nodiscard]] bool suspend(std::coroutine_handle<> handle) {
        auto& owner = state();
        auto* waiter = self();
        std::lock_guard lock(owner.mutex);
        if (!completion_.suspend(handle)) {
            return false;
        }
        if (timeout_) {
            try {
                WorkerHandleAccess::scheduleTimer(owner.worker, timer_,
                    workerTimerDeadlineAfter(*timeout_),
                    [&owner, waiter, completion = &completion_](WorkerTimerOutcome outcome) {
                        if (outcome == WorkerTimerOutcome::kExpired) {
                            std::lock_guard stateLock(owner.mutex);
                            if (owner.waiter == waiter) {
                                owner.waiter = nullptr;
                                owner.waiterGeneration = 0;
                                (void)completion->complete(WorkerWaitResultAccess::outcome<T>(
                                    WorkerWaitStatus::kTimedOut));
                            }
                        }
                        completion->continuation().resume();
                    });
            } catch (...) {
                if (owner.waiter == waiter) {
                    owner.waiter = nullptr;
                    owner.waiterGeneration = 0;
                }
                throw;
            }
        }
        return true;
    }

    [[nodiscard]] WorkerWaitResult<T> takeResult() {
        return completion_.takeValue();
    }

    // Completion has already detached the intrusive waiter before wake() runs.
    // A failed continuation dispatch is therefore unrecoverable and shares the
    // same explicit terminate boundary as WorkerSignal and timer cancellation.
    void wake() noexcept {
        if (timer_.registered()) {
            timer_.cancel();
            return;
        }
        auto* waiter = self();
        WorkerHandleAccess::deferOrTerminate(
            state().worker, [waiter] { waiter->resumeContinuation(); });
    }

    void resumeContinuation() noexcept {
        completion_.continuation().resume();
    }

private:
    [[nodiscard]] Awaiter* self() noexcept {
        return static_cast<Awaiter*>(this);
    }

    [[nodiscard]] static std::uint64_t reserveGeneration(State& state) noexcept {
        std::lock_guard lock(state.mutex);
        if (++state.nextWaiterGeneration == 0) {
            ++state.nextWaiterGeneration;
        }
        return state.nextWaiterGeneration;
    }

    [[nodiscard]] static StopRegistration registerCancellation(
        const StopToken& token, const std::shared_ptr<State>& state, std::uint64_t generation) {
        if (!token.stoppable() || token.stopRequested()) {
            return {};
        }
        return token.registerCallback([state, generation] {
            (void)WorkerHandleAccess::deferIfAttached(
                state->worker, [state, generation] { cancelWorkerSingleWait(state, generation); });
        });
    }

    std::shared_ptr<State> state_;
    std::optional<std::chrono::steady_clock::duration> timeout_;
    StopToken stopToken_;
    std::uint64_t generation_{0};
    WorkerTimerRegistration timer_;
    WorkerWaitAwaitState<T> completion_;
    // Last so callback unregistration completes before the state, timer, and
    // borrowed dispatcher begin destruction.
    StopRegistration stopRegistration_;
};

// Called with state.mutex held. Every terminal source uses this one transition,
// so detachment, generation invalidation, result publication, and wake ordering
// cannot drift between Channel and OneShot.
template <typename State>
void completeWorkerSingleWait(State& state, WorkerWaitStatus status) noexcept {
    auto* waiter = std::exchange(state.waiter, nullptr);
    state.waiterGeneration = 0;
    if (waiter != nullptr && waiter->completeStatus(status)) {
        waiter->wake();
    }
}

template <typename State>
void cancelWorkerSingleWait(
    const std::shared_ptr<State>& state, std::uint64_t generation) noexcept {
    std::lock_guard lock(state->mutex);
    if (state->waiter == nullptr || state->waiterGeneration != generation) {
        return;
    }
    completeWorkerSingleWait(*state, WorkerWaitStatus::kCancelled);
}

}  // namespace ruvia::detail
