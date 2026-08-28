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
#include <ruvia/core/StopToken.h>
#include <ruvia/core/WorkerHandle.h>
#include <ruvia/core/WorkerWaitResult.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>
#include <ruvia/core/detail/worker/WorkerTimer.h>
#include <ruvia/core/detail/worker/WorkerWaitAwaiter.h>
#include <ruvia/core/memory/PmrResource.h>

namespace ruvia {

enum class OneShotCompleteStatus : std::uint8_t {
    kCompleted,
    kAlreadyCompleted,
    kReceiverClosed,
    kWorkerStopping,
};

template <typename T>
class OneShotCompletion;

struct OneShotOptions final {
    std::pmr::memory_resource* resource{nullptr};
};

template <typename T>
class OneShotCompleteResult final {
public:
    OneShotCompleteResult(const OneShotCompleteResult&) = delete;
    OneShotCompleteResult& operator=(const OneShotCompleteResult&) = delete;
    OneShotCompleteResult(OneShotCompleteResult&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
    OneShotCompleteResult& operator=(OneShotCompleteResult&&) noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>) = default;

    [[nodiscard]] OneShotCompleteStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return status_ == OneShotCompleteStatus::kCompleted;
    }

    [[nodiscard]] T* rejected() & noexcept {
        return rejected_ ? &*rejected_ : nullptr;
    }

    [[nodiscard]] const T* rejected() const& noexcept {
        return rejected_ ? &*rejected_ : nullptr;
    }

    T* rejected() && = delete;
    const T* rejected() const&& = delete;

    [[nodiscard]] std::optional<T> takeRejected() && noexcept(std::is_nothrow_move_constructible_v<T>) {
        return std::move(rejected_);
    }

private:
    friend class OneShotCompletion<T>;

    explicit OneShotCompleteResult(OneShotCompleteStatus status) noexcept
        : status_(status) {}

    OneShotCompleteResult(OneShotCompleteStatus status, T&& rejected) noexcept(std::is_nothrow_move_constructible_v<T>)
        : status_(status),
          rejected_(std::move(rejected)) {}

    [[nodiscard]] static OneShotCompleteResult accept() noexcept {
        return OneShotCompleteResult(OneShotCompleteStatus::kCompleted);
    }

    [[nodiscard]] static OneShotCompleteResult reject(OneShotCompleteStatus status, T&& value) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return OneShotCompleteResult(status, std::move(value));
    }

    OneShotCompleteStatus status_;
    std::optional<T> rejected_;
};

template <typename T>
class OneShotReceiver;

namespace detail {

template <typename T>
struct OneShotAwaiter;

template <typename T>
struct OneShotState;

struct OneShotPending final {};
struct OneShotConsumed final {};
struct OneShotReceiverClosed final {};
struct OneShotWorkerStopping final {};

template <typename T>
class OneShotReady final {
public:
    explicit OneShotReady(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value)) {}

    [[nodiscard]] T takeValue() && noexcept(std::is_nothrow_move_constructible_v<T>) {
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
    using Lifecycle = std::variant<OneShotPending, OneShotReady<T>, OneShotConsumed, OneShotReceiverClosed, OneShotWorkerStopping>;
    Lifecycle lifecycle;
    OneShotAwaiter<T>* waiter{nullptr};
    std::uint64_t waiterGeneration{0};
    std::uint64_t nextWaiterGeneration{0};
};

template <typename T>
struct OneShotAwaiter final : WorkerSingleWaitAwaiter<T, OneShotState<T>, OneShotAwaiter<T>> {
    using Wait = WorkerSingleWaitAwaiter<T, OneShotState<T>, OneShotAwaiter<T>>;

    OneShotAwaiter(std::shared_ptr<OneShotState<T>> value, std::optional<std::chrono::steady_clock::duration> timeoutValue, StopToken stopTokenValue)
        : Wait(std::move(value), timeoutValue, std::move(stopTokenValue)) {}

    [[nodiscard]] bool await_ready() {
        auto& owner = this->state();
        std::lock_guard lock(owner.mutex);
        if (auto* ready = std::get_if<OneShotReady<T>>(&owner.lifecycle)) {
            (void)this->completeResult(WorkerWaitResultAccess::value(std::move(*ready).takeValue()));
            owner.lifecycle.template emplace<OneShotConsumed>();
            return true;
        }
        if (std::holds_alternative<OneShotWorkerStopping>(owner.lifecycle)) {
            (void)this->completeStatus(WorkerWaitStatus::kWorkerStopping);
            return true;
        }
        if (std::holds_alternative<OneShotReceiverClosed>(owner.lifecycle) || std::holds_alternative<OneShotConsumed>(owner.lifecycle)) {
            (void)this->completeStatus(WorkerWaitStatus::kClosed);
            return true;
        }
        assert(std::holds_alternative<OneShotPending>(owner.lifecycle));
        if (this->stopToken().stopRequested()) {
            (void)this->completeStatus(WorkerWaitStatus::kCancelled);
            return true;
        }
        const auto& timeout = this->timeout();
        if (timeout.has_value() && timeout.value() <= std::chrono::steady_clock::duration::zero()) {
            (void)this->completeStatus(WorkerWaitStatus::kTimedOut);
            return true;
        }
        if (owner.waiter != nullptr) {
            throw std::logic_error("one-shot supports one pending receiver");
        }
        this->publish();
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        return this->suspend(handle);
    }

    [[nodiscard]] WorkerWaitResult<T> await_resume() {
        return this->takeResult();
    }
};

template <typename T>
[[nodiscard]] Task<WorkerWaitResult<T>> waitOneShotState(std::shared_ptr<OneShotState<T>> state, std::optional<std::chrono::steady_clock::duration> timeout, StopToken stopToken) {
    if (!state || !state->worker.isCurrent()) {
        throw std::logic_error("one-shot wait must run on its bound worker");
    }
    co_return co_await OneShotAwaiter<T>(std::move(state), timeout, std::move(stopToken));
}

template <typename T>
void OneShotState<T>::workerStopping() noexcept {
    std::lock_guard lock(mutex);
    if (!std::holds_alternative<OneShotReady<T>>(lifecycle)) {
        lifecycle.template emplace<OneShotWorkerStopping>();
    }
    completeWorkerSingleWait(*this, WorkerWaitStatus::kWorkerStopping);
}

}  // namespace detail

template <typename T>
class OneShotCompletion final {
public:
    OneShotCompletion() noexcept = default;

    [[nodiscard]] OneShotCompleteResult<T> complete(T value) const {
        if (!state_) {
            return OneShotCompleteResult<T>::reject(OneShotCompleteStatus::kReceiverClosed, std::move(value));
        }
        detail::OneShotAwaiter<T>* waiter = nullptr;
        bool wake = false;
        {
            std::lock_guard lock(state_->mutex);
            if (std::holds_alternative<detail::OneShotReady<T>>(state_->lifecycle) || std::holds_alternative<detail::OneShotConsumed>(state_->lifecycle)) {
                return OneShotCompleteResult<T>::reject(OneShotCompleteStatus::kAlreadyCompleted, std::move(value));
            }
            if (std::holds_alternative<detail::OneShotWorkerStopping>(state_->lifecycle)) {
                return OneShotCompleteResult<T>::reject(OneShotCompleteStatus::kWorkerStopping, std::move(value));
            }
            if (std::holds_alternative<detail::OneShotReceiverClosed>(state_->lifecycle)) {
                return OneShotCompleteResult<T>::reject(OneShotCompleteStatus::kReceiverClosed, std::move(value));
            }
            assert(std::holds_alternative<detail::OneShotPending>(state_->lifecycle));
            if (!state_->worker.accepting()) {
                return OneShotCompleteResult<T>::reject(OneShotCompleteStatus::kWorkerStopping, std::move(value));
            }
            waiter = state_->waiter;
            if (waiter != nullptr) {
                wake = waiter->completeResult(detail::WorkerWaitResultAccess::value(std::move(value)));
                state_->lifecycle.template emplace<detail::OneShotConsumed>();
                state_->waiter = nullptr;
                state_->waiterGeneration = 0;
                // Wake the receiver while still holding the mutex. Once it is
                // released, an already-in-flight timer expiry can resume the
                // receiver on its worker and destroy this awaiter, so reading
                // its timer registration afterward would be a use-after-free.
                if (wake) {
                    waiter->wake();
                }
            } else {
                try {
                    state_->lifecycle.template emplace<detail::OneShotReady<T>>(std::move(value));
                } catch (...) {
                    state_->lifecycle.template emplace<detail::OneShotPending>();
                    throw;
                }
            }
        }
        return OneShotCompleteResult<T>::accept();
    }

private:
    explicit OneShotCompletion(std::shared_ptr<detail::OneShotState<T>> state)
        : state_(std::move(state)) {}
    std::shared_ptr<detail::OneShotState<T>> state_;
    template <typename U>
    friend auto makeOneShot(WorkerHandle, OneShotOptions);
};

template <typename T>
class OneShotReceiver final {
public:
    OneShotReceiver() = delete;
    OneShotReceiver(const OneShotReceiver&) = delete;
    OneShotReceiver& operator=(const OneShotReceiver&) = delete;
    OneShotReceiver(OneShotReceiver&&) noexcept = default;
    OneShotReceiver& operator=(OneShotReceiver&&) = delete;
    ~OneShotReceiver() {
        close();
    }

    // The task owns the shared state, but this receiver owns the receive side:
    // destroying it closes every cold or running wait. Require a named owner
    // so storing a task cannot silently turn a temporary receiver into an
    // already-closed operation.
    [[nodiscard]] Task<WorkerWaitResult<T>> wait() const& {
        return detail::waitOneShotState<T>(state_, std::nullopt, {});
    }
    Task<WorkerWaitResult<T>> wait() const&& = delete;

    [[nodiscard]] Task<WorkerWaitResult<T>> wait(StopToken stopToken) const& {
        return detail::waitOneShotState<T>(state_, std::nullopt, std::move(stopToken));
    }
    Task<WorkerWaitResult<T>> wait(StopToken) const&& = delete;

    template <typename Rep, typename Period>
    [[nodiscard]] Task<WorkerWaitResult<T>> waitFor(std::chrono::duration<Rep, Period> duration) const& {
        return detail::waitOneShotState<T>(state_, detail::workerTimerSaturatingDurationCast(duration), {});
    }
    template <typename Rep, typename Period>
    Task<WorkerWaitResult<T>> waitFor(std::chrono::duration<Rep, Period>) const&& = delete;

    template <typename Rep, typename Period>
    [[nodiscard]] Task<WorkerWaitResult<T>> waitFor(std::chrono::duration<Rep, Period> duration, StopToken stopToken) const& {
        return detail::waitOneShotState<T>(state_, detail::workerTimerSaturatingDurationCast(duration), std::move(stopToken));
    }
    template <typename Rep, typename Period>
    Task<WorkerWaitResult<T>> waitFor(std::chrono::duration<Rep, Period>, StopToken) const&& = delete;

    // The worker every wait must run on. Makes the receive-side affinity
    // contract queryable instead of only failing at await time. A moved-from
    // receiver has no bound worker and cannot answer this query.
    [[nodiscard]] const WorkerHandle& worker() const& noexcept {
        if (!state_) {
            std::terminate();
        }
        return state_->worker;
    }
    const WorkerHandle& worker() const&& = delete;

    void close() const {
        if (!state_) {
            return;
        }
        detail::OneShotAwaiter<T>* waiter = nullptr;
        bool wake = false;
        {
            std::lock_guard lock(state_->mutex);
            if (!std::holds_alternative<detail::OneShotPending>(state_->lifecycle)) {
                return;
            }
            state_->lifecycle.template emplace<detail::OneShotReceiverClosed>();
            waiter = std::exchange(state_->waiter, nullptr);
            state_->waiterGeneration = 0;
            if (waiter != nullptr) {
                wake = waiter->completeStatus(WorkerWaitStatus::kClosed);
                // Wake under the mutex (see OneShotCompletion::complete).
                if (wake) {
                    waiter->wake();
                }
            }
        }
    }

private:
    explicit OneShotReceiver(std::shared_ptr<detail::OneShotState<T>> state)
        : state_(std::move(state)) {}
    std::shared_ptr<detail::OneShotState<T>> state_;
    template <typename U>
    friend auto makeOneShot(WorkerHandle, OneShotOptions);
};

template <typename T>
// The value parameter accepts both lvalue handles and rvalue handles before
// transferring the stable dispatcher endpoint into the one-shot state.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
[[nodiscard]] auto makeOneShot(WorkerHandle worker, OneShotOptions options = {}) {
    auto* resolved = detail::pmrResourceOrDefault(options.resource);
    std::pmr::polymorphic_allocator<detail::OneShotState<T>> allocator(resolved);
    auto state = std::allocate_shared<detail::OneShotState<T>>(allocator, std::move(worker));
    detail::WorkerHandleAccess::registerShutdownListener(state->worker, state);
    return std::pair(OneShotCompletion<T>(state), OneShotReceiver<T>(state));
}

}  // namespace ruvia
