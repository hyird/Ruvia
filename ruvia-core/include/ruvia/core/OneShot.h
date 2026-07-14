#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>
#include <ruvia/core/detail/WorkerDispatcher.h>
#include <ruvia/core/detail/WorkerTimer.h>
#include <ruvia/core/memory/PmrResource.h>

namespace ruvia {

enum class OneShotCompleteResult : std::uint8_t {
    kCompleted,
    kAlreadyCompleted,
    kReceiverClosed,
    kWorkerStopping,
};

enum class OneShotWaitStatus : std::uint8_t {
    kValue,
    kTimeout,
    kClosed,
    kWorkerStopping,
};

template <typename T>
struct OneShotWaitResult final {
    OneShotWaitStatus status{OneShotWaitStatus::kClosed};
    std::optional<T> value;
};

template <typename T>
class OneShotCompletion;
template <typename T>
class OneShotReceiver;

namespace detail {

template <typename T>
struct OneShotAwaiter;

template <typename T>
struct OneShotState final : WorkerShutdownListener {
    OneShotState(WorkerHandle target, std::pmr::memory_resource* memory)
        : worker(std::move(target)), resource(memory) {
        if (!worker.valid()) {
            throw std::invalid_argument("one-shot requires a valid worker");
        }
    }

    void workerStopping() noexcept override;

    WorkerHandle worker;
    std::pmr::memory_resource* resource;
    std::mutex mutex;
    std::optional<T> value;
    OneShotAwaiter<T>* waiter{nullptr};
    bool completed{false};
    bool consumed{false};
    bool closed{false};
    bool workerStopped{false};
};

template <typename T>
struct OneShotAwaiter final {
    OneShotAwaiter(std::shared_ptr<OneShotState<T>> value,
                   std::optional<std::chrono::steady_clock::duration> timeoutValue)
        : state(std::move(value)), timeout(timeoutValue) {}

    [[nodiscard]] bool await_ready() {
        std::lock_guard lock(state->mutex);
        if (state->completed && !state->consumed) {
            result.status = OneShotWaitStatus::kValue;
            result.value.emplace(std::move(*state->value));
            state->value.reset();
            state->consumed = true;
            return true;
        }
        if (state->workerStopped) {
            result.status = OneShotWaitStatus::kWorkerStopping;
            return true;
        }
        if (state->closed || state->consumed) {
            result.status = OneShotWaitStatus::kClosed;
            return true;
        }
        if (timeout && *timeout <= std::chrono::steady_clock::duration::zero()) {
            result.status = OneShotWaitStatus::kTimeout;
            return true;
        }
        if (state->waiter != nullptr) {
            throw std::logic_error("one-shot supports one pending receiver");
        }
        state->waiter = this;
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        continuation = handle;
        std::lock_guard lock(state->mutex);
        suspended = true;
        if (wakePending) {
            return false;
        }
        if (timeout) {
            try {
                timer = WorkerHandleAccess::scheduleTimer(
                    state->worker, std::chrono::steady_clock::now() + *timeout,
                    [this](bool cancelled) {
                        if (!cancelled) {
                            std::lock_guard stateLock(state->mutex);
                            if (state->waiter == this) {
                                state->waiter = nullptr;
                                result.status = OneShotWaitStatus::kTimeout;
                            }
                        }
                        continuation.resume();
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

    [[nodiscard]] OneShotWaitResult<T> await_resume() { return std::move(result); }

    std::shared_ptr<OneShotState<T>> state;
    std::optional<std::chrono::steady_clock::duration> timeout;
    WorkerTimerRegistration timer;
    OneShotWaitResult<T> result;
    std::coroutine_handle<> continuation{};
    bool suspended{false};
    bool wakePending{false};
};

template <typename T>
void wakeOneShot(OneShotAwaiter<T>* waiter) {
    if (!waiter->suspended) {
        waiter->wakePending = true;
        return;
    }
    if (waiter->timer.valid()) {
        waiter->timer.cancel();
        return;
    }
    WorkerHandleAccess::defer(
        waiter->state->worker, [waiter] { waiter->continuation.resume(); });
}

template <typename T>
void OneShotState<T>::workerStopping() noexcept {
    OneShotAwaiter<T>* pending = nullptr;
    {
        std::lock_guard lock(mutex);
        closed = true;
        workerStopped = true;
        pending = std::exchange(waiter, nullptr);
        if (pending != nullptr) {
            pending->result.status = OneShotWaitStatus::kWorkerStopping;
            try {
                wakeOneShot(pending);
            } catch (...) {
                std::terminate();
            }
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
        {
            std::lock_guard lock(state_->mutex);
            if (state_->completed) {
                return OneShotCompleteResult::kAlreadyCompleted;
            }
            if (state_->workerStopped) {
                return OneShotCompleteResult::kWorkerStopping;
            }
            if (state_->closed) {
                return OneShotCompleteResult::kReceiverClosed;
            }
            if (!state_->worker.accepting()) {
                return OneShotCompleteResult::kWorkerStopping;
            }
            state_->completed = true;
            waiter = std::exchange(state_->waiter, nullptr);
            if (waiter != nullptr) {
                waiter->result.status = OneShotWaitStatus::kValue;
                waiter->result.value.emplace(std::move(value));
                state_->consumed = true;
                detail::wakeOneShot(waiter);
            } else {
                state_->value.emplace(std::move(value));
            }
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
    OneShotReceiver() noexcept = default;
    OneShotReceiver(const OneShotReceiver&) = delete;
    OneShotReceiver& operator=(const OneShotReceiver&) = delete;
    OneShotReceiver(OneShotReceiver&&) noexcept = default;
    OneShotReceiver& operator=(OneShotReceiver&&) noexcept = default;
    ~OneShotReceiver() { close(); }

    [[nodiscard]] Task<OneShotWaitResult<T>> wait() {
        validateWorker();
        co_return co_await detail::OneShotAwaiter<T>(state_, std::nullopt);
    }

    template <typename Rep, typename Period>
    [[nodiscard]] Task<OneShotWaitResult<T>>
    waitFor(std::chrono::duration<Rep, Period> duration) {
        validateWorker();
        co_return co_await detail::OneShotAwaiter<T>(
            state_, std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration));
    }

    void close() const {
        if (!state_) {
            return;
        }
        std::lock_guard lock(state_->mutex);
        if (state_->closed) {
            return;
        }
        state_->closed = true;
        auto* waiter = std::exchange(state_->waiter, nullptr);
        if (waiter != nullptr) {
            waiter->result.status = OneShotWaitStatus::kClosed;
            detail::wakeOneShot(waiter);
        }
    }

private:
    explicit OneShotReceiver(std::shared_ptr<detail::OneShotState<T>> state)
        : state_(std::move(state)) {}
    void validateWorker() const {
        if (!state_ || !state_->worker.isCurrent()) {
            throw std::logic_error("one-shot wait must run on its bound worker");
        }
    }
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
        allocator, std::move(worker), resolved);
    detail::WorkerHandleAccess::registerShutdownListener(state->worker, state);
    return std::pair(OneShotCompletion<T>(state), OneShotReceiver<T>(state));
}

}
