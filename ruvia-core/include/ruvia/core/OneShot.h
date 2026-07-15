#pragma once

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>
#include <ruvia/core/WorkerWaitResult.h>
#include <ruvia/core/detail/WorkerDispatcher.h>
#include <ruvia/core/detail/WorkerTimer.h>
#include <ruvia/core/detail/WorkerWaitAwaiter.h>
#include <ruvia/core/memory/PmrResource.h>

namespace ruvia {

enum class OneShotCompleteResult : std::uint8_t {
    kCompleted,
    kAlreadyCompleted,
    kReceiverClosed,
    kWorkerStopping,
};

template <typename T>
class OneShotCompletion;
template <typename T>
class OneShotReceiver;

namespace detail {

template <typename T>
struct OneShotAwaiter;

struct OneShotPending final {};
struct OneShotConsumed final {};
struct OneShotReceiverClosed final {};
struct OneShotWorkerStopping final {};

template <typename T>
class OneShotReady final {
public:
    explicit OneShotReady(T&& value)
        noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value)) {}

    [[nodiscard]] T takeValue() &&
        noexcept(std::is_nothrow_move_constructible_v<T>) {
        return std::move(value_);
    }

private:
    T value_;
};

template <typename T>
struct OneShotState final : WorkerShutdownListener {
    explicit OneShotState(WorkerHandle target)
        : worker(std::move(target)) {
        if (!worker.valid()) {
            throw std::invalid_argument("one-shot requires a valid worker");
        }
    }

    void workerStopping() noexcept override;

    WorkerHandle worker;
    std::mutex mutex;
    using Lifecycle = std::variant<
        OneShotPending,
        OneShotReady<T>,
        OneShotConsumed,
        OneShotReceiverClosed,
        OneShotWorkerStopping>;
    Lifecycle lifecycle;
    OneShotAwaiter<T>* waiter{nullptr};
};

template <typename T>
struct OneShotAwaiter final {
    OneShotAwaiter(std::shared_ptr<OneShotState<T>> value,
                   std::optional<std::chrono::steady_clock::duration> timeoutValue)
        : state(std::move(value)), timeout(timeoutValue) {}

    [[nodiscard]] bool await_ready() {
        std::lock_guard lock(state->mutex);
        if (auto* ready = std::get_if<OneShotReady<T>>(&state->lifecycle)) {
            (void)completion.complete(WorkerWaitResultAccess::value(
                std::move(*ready).takeValue()));
            state->lifecycle.template emplace<OneShotConsumed>();
            return true;
        }
        if (std::holds_alternative<OneShotWorkerStopping>(state->lifecycle)) {
            (void)completion.complete(
                WorkerWaitResultAccess::workerStopping<T>());
            return true;
        }
        if (std::holds_alternative<OneShotReceiverClosed>(state->lifecycle) ||
            std::holds_alternative<OneShotConsumed>(state->lifecycle)) {
            (void)completion.complete(WorkerWaitResultAccess::closed<T>());
            return true;
        }
        assert(std::holds_alternative<OneShotPending>(state->lifecycle));
        if (timeout && *timeout <= std::chrono::steady_clock::duration::zero()) {
            (void)completion.complete(WorkerWaitResultAccess::timedOut<T>());
            return true;
        }
        if (state->waiter != nullptr) {
            throw std::logic_error("one-shot supports one pending receiver");
        }
        state->waiter = this;
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        std::lock_guard lock(state->mutex);
        if (!completion.suspend(handle)) {
            return false;
        }
        if (timeout) {
            try {
                WorkerHandleAccess::scheduleTimer(
                    state->worker, timer,
                    std::chrono::steady_clock::now() + *timeout,
                    [this](WorkerTimerOutcome outcome) {
                        if (outcome == WorkerTimerOutcome::kExpired) {
                            std::lock_guard stateLock(state->mutex);
                            if (state->waiter == this) {
                                state->waiter = nullptr;
                                (void)completion.complete(
                                    WorkerWaitResultAccess::timedOut<T>());
                            }
                        }
                        completion.continuation().resume();
                    });
            } catch (...) {
                if (state->waiter == this) {
                    state->waiter = nullptr;
                }
                throw;
            }
        }
        return true;
    }

    [[nodiscard]] WorkerWaitResult<T> await_resume() {
        return completion.takeResult();
    }

    std::shared_ptr<OneShotState<T>> state;
    std::optional<std::chrono::steady_clock::duration> timeout;
    WorkerTimerRegistration timer;
    WorkerWaitAwaitState<T> completion;
};

template <typename T>
[[nodiscard]] Task<WorkerWaitResult<T>> waitOneShotState(
    std::shared_ptr<OneShotState<T>> state,
    std::optional<std::chrono::steady_clock::duration> timeout) {
    if (!state || !state->worker.isCurrent()) {
        throw std::logic_error("one-shot wait must run on its bound worker");
    }
    co_return co_await OneShotAwaiter<T>(std::move(state), timeout);
}

template <typename T>
void wakeOneShotReceiver(OneShotAwaiter<T>* waiter) {
    if (waiter->timer.registered()) {
        waiter->timer.cancel();
        return;
    }
    WorkerHandleAccess::defer(
        waiter->state->worker,
        [waiter] { waiter->completion.continuation().resume(); });
}

template <typename T>
void OneShotState<T>::workerStopping() noexcept {
    OneShotAwaiter<T>* pending = nullptr;
    bool wake = false;
    {
        std::lock_guard lock(mutex);
        if (!std::holds_alternative<OneShotReady<T>>(lifecycle)) {
            lifecycle.template emplace<OneShotWorkerStopping>();
        }
        pending = std::exchange(waiter, nullptr);
        if (pending != nullptr) {
            wake = pending->completion.complete(
                WorkerWaitResultAccess::workerStopping<T>());
        }
    }
    if (wake) {
        try {
            wakeOneShotReceiver(pending);
        } catch (...) {
            std::terminate();
        }
    }
}

}

template <typename T>
class OneShotCompletion final {
public:
    OneShotCompletion() noexcept = default;

    [[nodiscard]] OneShotCompleteResult complete(T value) const {
        if (!state_) {
            return OneShotCompleteResult::kReceiverClosed;
        }
        detail::OneShotAwaiter<T>* waiter = nullptr;
        bool wake = false;
        {
            std::lock_guard lock(state_->mutex);
            if (std::holds_alternative<detail::OneShotReady<T>>(state_->lifecycle) ||
                std::holds_alternative<detail::OneShotConsumed>(state_->lifecycle)) {
                return OneShotCompleteResult::kAlreadyCompleted;
            }
            if (std::holds_alternative<detail::OneShotWorkerStopping>(state_->lifecycle)) {
                return OneShotCompleteResult::kWorkerStopping;
            }
            if (std::holds_alternative<detail::OneShotReceiverClosed>(state_->lifecycle)) {
                return OneShotCompleteResult::kReceiverClosed;
            }
            assert(std::holds_alternative<detail::OneShotPending>(state_->lifecycle));
            if (!state_->worker.accepting()) {
                return OneShotCompleteResult::kWorkerStopping;
            }
            waiter = state_->waiter;
            if (waiter != nullptr) {
                wake = waiter->completion.complete(
                    detail::WorkerWaitResultAccess::value(std::move(value)));
                state_->lifecycle.template emplace<detail::OneShotConsumed>();
                state_->waiter = nullptr;
            } else {
                try {
                    state_->lifecycle.template emplace<detail::OneShotReady<T>>(
                        std::move(value));
                } catch (...) {
                    state_->lifecycle.template emplace<detail::OneShotPending>();
                    throw;
                }
            }
        }
        if (wake) {
            detail::wakeOneShotReceiver(waiter);
        }
        return OneShotCompleteResult::kCompleted;
    }

private:
    explicit OneShotCompletion(std::shared_ptr<detail::OneShotState<T>> state)
        : state_(std::move(state)) {}
    std::shared_ptr<detail::OneShotState<T>> state_;
    template <typename U>
    friend auto makeOneShot(WorkerHandle, std::pmr::memory_resource*);
};

template <typename T>
class OneShotReceiver final {
public:
    OneShotReceiver() = delete;
    OneShotReceiver(const OneShotReceiver&) = delete;
    OneShotReceiver& operator=(const OneShotReceiver&) = delete;
    OneShotReceiver(OneShotReceiver&&) noexcept = default;
    OneShotReceiver& operator=(OneShotReceiver&&) = delete;
    ~OneShotReceiver() { close(); }

    [[nodiscard]] Task<WorkerWaitResult<T>> wait() {
        return detail::waitOneShotState<T>(state_, std::nullopt);
    }

    template <typename Rep, typename Period>
    [[nodiscard]] Task<WorkerWaitResult<T>>
    waitFor(std::chrono::duration<Rep, Period> duration) {
        return detail::waitOneShotState<T>(
            state_,
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                duration));
    }

    void close() const {
        if (!state_) {
            return;
        }
        detail::OneShotAwaiter<T>* waiter = nullptr;
        bool wake = false;
        {
            std::lock_guard lock(state_->mutex);
            if (!std::holds_alternative<detail::OneShotPending>(
                    state_->lifecycle)) {
                return;
            }
            state_->lifecycle.template emplace<detail::OneShotReceiverClosed>();
            waiter = std::exchange(state_->waiter, nullptr);
            if (waiter != nullptr) {
                wake = waiter->completion.complete(
                    detail::WorkerWaitResultAccess::closed<T>());
            }
        }
        if (wake) {
            detail::wakeOneShotReceiver(waiter);
        }
    }

private:
    explicit OneShotReceiver(std::shared_ptr<detail::OneShotState<T>> state)
        : state_(std::move(state)) {}
    std::shared_ptr<detail::OneShotState<T>> state_;
    template <typename U>
    friend auto makeOneShot(WorkerHandle, std::pmr::memory_resource*);
};

template <typename T>
[[nodiscard]] auto makeOneShot(WorkerHandle worker,
                               std::pmr::memory_resource* resource = nullptr) {
    auto* resolved = detail::pmrResourceOrDefault(resource);
    std::pmr::polymorphic_allocator<detail::OneShotState<T>> allocator(resolved);
    auto state = std::allocate_shared<detail::OneShotState<T>>(
        allocator, std::move(worker));
    detail::WorkerHandleAccess::registerShutdownListener(state->worker, state);
    return std::pair(OneShotCompletion<T>(state), OneShotReceiver<T>(state));
}

}
