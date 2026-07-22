#pragma once

#include <chrono>
#include <coroutine>
#include <exception>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include <ruvia/core/WorkerWaitResult.h>

namespace ruvia::detail {

struct WorkerWaitAwaitPreparing final {};

class WorkerWaitAwaitSuspended final {
public:
    explicit WorkerWaitAwaitSuspended(
        std::coroutine_handle<> continuation) noexcept
        : continuation_(continuation) {}

    [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
        return continuation_;
    }

private:
    std::coroutine_handle<> continuation_;
};

template <typename T>
class WorkerWaitAwaitReadyBeforeSuspend final {
public:
    explicit WorkerWaitAwaitReadyBeforeSuspend(WorkerWaitResult<T>&& result)
        noexcept(std::is_nothrow_move_constructible_v<WorkerWaitResult<T>>)
        : result_(std::move(result)) {}

    [[nodiscard]] WorkerWaitResult<T> takeResult() &&
        noexcept(std::is_nothrow_move_constructible_v<WorkerWaitResult<T>>) {
        return std::move(result_);
    }

private:
    WorkerWaitResult<T> result_;
};

template <typename T>
class WorkerWaitAwaitReadyAfterSuspend final {
public:
    WorkerWaitAwaitReadyAfterSuspend(
        WorkerWaitResult<T>&& result,
        std::coroutine_handle<> continuation)
        noexcept(std::is_nothrow_move_constructible_v<WorkerWaitResult<T>>)
        : result_(std::move(result)), continuation_(continuation) {}

    [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
        return continuation_;
    }

    [[nodiscard]] WorkerWaitResult<T> takeResult() &&
        noexcept(std::is_nothrow_move_constructible_v<WorkerWaitResult<T>>) {
        return std::move(result_);
    }

private:
    WorkerWaitResult<T> result_;
    std::coroutine_handle<> continuation_;
};

// A receiver can complete either before or after await_suspend. Keeping those
// paths distinct prevents an optional result, a continuation, and parallel
// suspended/wake-pending flags from describing contradictory states.
template <typename T>
class WorkerWaitAwaitState final {
public:
    WorkerWaitAwaitState() = default;
    WorkerWaitAwaitState(const WorkerWaitAwaitState&) = delete;
    WorkerWaitAwaitState& operator=(const WorkerWaitAwaitState&) = delete;
    WorkerWaitAwaitState(WorkerWaitAwaitState&&) = delete;
    WorkerWaitAwaitState& operator=(WorkerWaitAwaitState&&) = delete;

    [[nodiscard]] bool suspend(
        std::coroutine_handle<> continuation) noexcept {
        if (std::holds_alternative<WorkerWaitAwaitPreparing>(state_)) {
            state_.template emplace<WorkerWaitAwaitSuspended>(continuation);
            return true;
        }
        if (std::holds_alternative<WorkerWaitAwaitReadyBeforeSuspend<T>>(state_)) {
            return false;
        }
        std::terminate();
    }

    // Returns true only when the suspended coroutine now needs an explicit
    // wake. A completion racing before await_suspend is consumed there instead.
    [[nodiscard]] bool complete(WorkerWaitResult<T>&& result) {
        if (std::holds_alternative<WorkerWaitAwaitPreparing>(state_)) {
            try {
                state_.template emplace<WorkerWaitAwaitReadyBeforeSuspend<T>>(
                    std::move(result));
            } catch (...) {
                state_.template emplace<WorkerWaitAwaitPreparing>();
                throw;
            }
            return false;
        }
        if (auto* suspended =
                std::get_if<WorkerWaitAwaitSuspended>(&state_)) {
            const auto continuation = suspended->continuation();
            try {
                state_.template emplace<WorkerWaitAwaitReadyAfterSuspend<T>>(
                    std::move(result), continuation);
            } catch (...) {
                state_.template emplace<WorkerWaitAwaitSuspended>(continuation);
                throw;
            }
            return true;
        }
        std::terminate();
    }

    [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
        const auto* ready =
            std::get_if<WorkerWaitAwaitReadyAfterSuspend<T>>(&state_);
        if (ready == nullptr) {
            std::terminate();
        }
        return ready->continuation();
    }

    [[nodiscard]] WorkerWaitResult<T> takeResult()
        noexcept(std::is_nothrow_move_constructible_v<WorkerWaitResult<T>>) {
        if (auto* ready =
                std::get_if<WorkerWaitAwaitReadyBeforeSuspend<T>>(&state_)) {
            return std::move(*ready).takeResult();
        }
        if (auto* ready =
                std::get_if<WorkerWaitAwaitReadyAfterSuspend<T>>(&state_)) {
            return std::move(*ready).takeResult();
        }
        std::terminate();
    }

private:
    using State = std::variant<
        WorkerWaitAwaitPreparing,
        WorkerWaitAwaitSuspended,
        WorkerWaitAwaitReadyBeforeSuspend<T>,
        WorkerWaitAwaitReadyAfterSuspend<T>>;

    State state_;
};


// The suspend half both worker waits share: publish the continuation under the
// state's mutex, then arm the optional timeout. On expiry the waiter detaches
// itself from the state and completes as timed out, so a later completion finds
// no waiter; if arming throws, the waiter is detached before the throw
// propagates rather than being left published against a timer that never runs.
//
// `State` must expose `mutex`, `worker` and a `waiter` pointer comparable to
// `self` -- the contract ChannelState and OneShotState both satisfy.
template <typename T, typename State, typename Waiter>
[[nodiscard]] bool suspendWorkerWait(
    State& state,
    Waiter* self,
    WorkerWaitAwaitState<T>& completion,
    WorkerTimerRegistration& timer,
    const std::optional<std::chrono::steady_clock::duration>& timeout,
    std::coroutine_handle<> handle) {
    std::lock_guard lock(state.mutex);
    if (!completion.suspend(handle)) {
        return false;
    }
    if (timeout) {
        try {
            WorkerHandleAccess::scheduleTimer(
                state.worker, timer,
                workerTimerDeadlineAfter(*timeout),
                [&state, self, &completion](WorkerTimerOutcome outcome) {
                    if (outcome == WorkerTimerOutcome::kExpired) {
                        std::lock_guard stateLock(state.mutex);
                        if (state.waiter == self) {
                            state.waiter = nullptr;
                            (void)completion.complete(
                                WorkerWaitResultAccess::timedOut<T>());
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
