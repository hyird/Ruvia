#pragma once

#include <cassert>
#include <coroutine>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>
#include <ruvia/core/WorkerWaitResult.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>
#include <ruvia/core/detail/worker/WorkerTimer.h>
#include <ruvia/core/detail/worker/WorkerWaitAwaiter.h>
#include <ruvia/core/memory/PmrResource.h>

namespace ruvia {

enum class ChannelSendStatus : std::uint8_t {
    kSent,
    kFull,
    kClosed,
    kWorkerStopping,
};

template <typename T>
class ChannelSender;

template <typename T>
class ChannelSendResult final {
public:
    ChannelSendResult(const ChannelSendResult&) = delete;
    ChannelSendResult& operator=(const ChannelSendResult&) = delete;
    ChannelSendResult(ChannelSendResult&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
    ChannelSendResult& operator=(ChannelSendResult&&) noexcept(std::is_nothrow_move_assignable_v<T>) = default;

    [[nodiscard]] ChannelSendStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return status_ == ChannelSendStatus::kSent;
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
    friend class ChannelSender<T>;

    explicit ChannelSendResult(ChannelSendStatus status) noexcept
        : status_(status) {}

    ChannelSendResult(ChannelSendStatus status, T&& rejected) noexcept(std::is_nothrow_move_constructible_v<T>)
        : status_(status),
          rejected_(std::move(rejected)) {}

    [[nodiscard]] static ChannelSendResult accept() noexcept {
        return ChannelSendResult(ChannelSendStatus::kSent);
    }

    [[nodiscard]] static ChannelSendResult reject(ChannelSendStatus status, T&& value) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return ChannelSendResult(status, std::move(value));
    }

    ChannelSendStatus status_;
    std::optional<T> rejected_;
};

template <typename T>
class ChannelReceiver;

namespace detail {

template <typename T>
struct ChannelReceiveAwaiter;

struct ChannelOpen final {};
struct ChannelClosed final {};
struct ChannelWorkerStopping final {};

template <typename T>
struct ChannelState final : WorkerShutdownListener {
    ChannelState(WorkerHandle target, std::size_t requestedCapacity, std::pmr::memory_resource* resource)
        : worker(std::move(target)),
          slots(resource) {
        if (!worker.valid()) {
            throw std::invalid_argument("channel requires a valid worker");
        }
        if (requestedCapacity == 0) {
            throw std::invalid_argument("channel capacity must be greater than zero");
        }
        slots.resize(requestedCapacity);
    }

    WorkerHandle worker;
    std::pmr::vector<std::optional<T>> slots;
    std::mutex mutex;
    std::size_t head{0};
    std::size_t tail{0};
    std::size_t size{0};
    using Lifecycle = std::variant<ChannelOpen, ChannelClosed, ChannelWorkerStopping>;
    Lifecycle lifecycle;
    ChannelReceiveAwaiter<T>* waiter{nullptr};

    void workerStopping() noexcept override;
};

template <typename T>
struct ChannelReceiveAwaiter final {
    ChannelReceiveAwaiter(std::shared_ptr<ChannelState<T>> value, std::optional<std::chrono::steady_clock::duration> timeoutValue = std::nullopt)
        : state(std::move(value)),
          timeout(timeoutValue) {}

    [[nodiscard]] bool await_ready() {
        std::lock_guard lock(state->mutex);
        if (state->size != 0) {
            (void)completion.complete(WorkerWaitResultAccess::value(std::move(*state->slots[state->head])));
            state->slots[state->head].reset();
            state->head = (state->head + 1) % state->slots.size();
            --state->size;
            return true;
        }
        if (std::holds_alternative<ChannelWorkerStopping>(state->lifecycle)) {
            (void)completion.complete(WorkerWaitResultAccess::workerStopping<T>());
            return true;
        }
        if (std::holds_alternative<ChannelClosed>(state->lifecycle)) {
            (void)completion.complete(WorkerWaitResultAccess::closed<T>());
            return true;
        }
        assert(std::holds_alternative<ChannelOpen>(state->lifecycle));
        if (timeout && *timeout <= std::chrono::steady_clock::duration::zero()) {
            (void)completion.complete(WorkerWaitResultAccess::timedOut<T>());
            return true;
        }
        if (state->waiter != nullptr) {
            throw std::logic_error("channel supports one pending receiver");
        }
        state->waiter = this;
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        return suspendWorkerWait(*state, this, completion, timer, timeout, handle);
    }

    [[nodiscard]] WorkerWaitResult<T> await_resume() {
        return completion.takeValue();
    }

    std::shared_ptr<ChannelState<T>> state;
    std::optional<std::chrono::steady_clock::duration> timeout;
    WorkerTimerRegistration timer;
    WorkerWaitAwaitState<T> completion;
};

template <typename T>
[[nodiscard]] Task<WorkerWaitResult<T>> receiveChannelState(std::shared_ptr<ChannelState<T>> state, std::optional<std::chrono::steady_clock::duration> timeout) {
    if (!state || !state->worker.isCurrent()) {
        throw std::logic_error("channel receive must run on its bound worker");
    }
    co_return co_await ChannelReceiveAwaiter<T>(std::move(state), timeout);
}

template <typename T>
void wakeChannelReceiver(ChannelReceiveAwaiter<T>* waiter) {
    if (waiter->timer.registered()) {
        waiter->timer.cancel();
        return;
    }
    WorkerHandleAccess::defer(waiter->state->worker, [waiter] { waiter->completion.continuation().resume(); });
}

template <typename T>
void ChannelState<T>::workerStopping() noexcept {
    std::lock_guard lock(mutex);
    lifecycle.template emplace<ChannelWorkerStopping>();
    ChannelReceiveAwaiter<T>* pending = std::exchange(waiter, nullptr);
    if (pending != nullptr && pending->completion.complete(WorkerWaitResultAccess::workerStopping<T>())) {
        // Wake under the mutex (see ChannelSender::send).
        try {
            wakeChannelReceiver(pending);
        } catch (...) {
            std::terminate();
        }
    }
}

}  // namespace detail

template <typename T>
class ChannelSender final {
public:
    ChannelSender() noexcept = default;

    [[nodiscard]] ChannelSendResult<T> send(T value) const {
        if (!state_) {
            return ChannelSendResult<T>::reject(ChannelSendStatus::kClosed, std::move(value));
        }
        detail::ChannelReceiveAwaiter<T>* waiter = nullptr;
        bool wake = false;
        {
            std::lock_guard lock(state_->mutex);
            if (std::holds_alternative<detail::ChannelClosed>(state_->lifecycle)) {
                return ChannelSendResult<T>::reject(ChannelSendStatus::kClosed, std::move(value));
            }
            if (std::holds_alternative<detail::ChannelWorkerStopping>(state_->lifecycle)) {
                return ChannelSendResult<T>::reject(ChannelSendStatus::kWorkerStopping, std::move(value));
            }
            assert(std::holds_alternative<detail::ChannelOpen>(state_->lifecycle));
            if (!state_->worker.accepting()) {
                return ChannelSendResult<T>::reject(ChannelSendStatus::kWorkerStopping, std::move(value));
            }
            if (state_->waiter != nullptr) {
                waiter = state_->waiter;
                wake = waiter->completion.complete(detail::WorkerWaitResultAccess::value(std::move(value)));
                state_->waiter = nullptr;
                // Wake the receiver while still holding the mutex. Once it is
                // released, an already-in-flight timer expiry can resume the
                // receiver on its worker and destroy this awaiter, so reading
                // waiter->timer afterward would be a use-after-free.
                if (wake) {
                    detail::wakeChannelReceiver(waiter);
                }
            } else {
                if (state_->size == state_->slots.size()) {
                    return ChannelSendResult<T>::reject(ChannelSendStatus::kFull, std::move(value));
                }
                state_->slots[state_->tail].emplace(std::move(value));
                state_->tail = (state_->tail + 1) % state_->slots.size();
                ++state_->size;
            }
        }
        return ChannelSendResult<T>::accept();
    }

    void close() const {
        if (!state_) {
            return;
        }
        detail::ChannelReceiveAwaiter<T>* waiter = nullptr;
        bool wake = false;
        {
            std::lock_guard lock(state_->mutex);
            if (!std::holds_alternative<detail::ChannelOpen>(state_->lifecycle)) {
                return;
            }
            state_->lifecycle.template emplace<detail::ChannelClosed>();
            waiter = std::exchange(state_->waiter, nullptr);
            if (waiter != nullptr) {
                wake = waiter->completion.complete(detail::WorkerWaitResultAccess::closed<T>());
                // Wake under the mutex (see ChannelSender::send).
                if (wake) {
                    detail::wakeChannelReceiver(waiter);
                }
            }
        }
    }

private:
    explicit ChannelSender(std::shared_ptr<detail::ChannelState<T>> state)
        : state_(std::move(state)) {}

    std::shared_ptr<detail::ChannelState<T>> state_;
    friend class ChannelReceiver<T>;
    template <typename U>
    friend auto makeChannel(WorkerHandle, std::size_t, std::pmr::memory_resource*);
};

template <typename T>
class ChannelReceiver final {
public:
    ChannelReceiver() = delete;
    ChannelReceiver(const ChannelReceiver&) = delete;
    ChannelReceiver& operator=(const ChannelReceiver&) = delete;
    ChannelReceiver(ChannelReceiver&&) noexcept = default;
    ChannelReceiver& operator=(ChannelReceiver&&) = delete;

    ~ChannelReceiver() {
        close();
    }

    [[nodiscard]] Task<WorkerWaitResult<T>> receive() const {
        return detail::receiveChannelState<T>(state_, std::nullopt);
    }

    template <typename Rep, typename Period>
    [[nodiscard]] Task<WorkerWaitResult<T>> receiveFor(std::chrono::duration<Rep, Period> duration) const {
        return detail::receiveChannelState<T>(state_, detail::workerTimerSaturatingDurationCast(duration));
    }

    // The worker every receive must run on. Makes the receive-side affinity
    // contract queryable instead of only failing at await time. A moved-from
    // receiver has no bound worker and cannot answer this query.
    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        if (!state_) {
            std::terminate();
        }
        return state_->worker;
    }

    void close() const {
        ChannelSender<T>(state_).close();
    }

private:
    explicit ChannelReceiver(std::shared_ptr<detail::ChannelState<T>> state)
        : state_(std::move(state)) {}

    std::shared_ptr<detail::ChannelState<T>> state_;
    template <typename U>
    friend auto makeChannel(WorkerHandle, std::size_t, std::pmr::memory_resource*);
};

template <typename T>
[[nodiscard]] auto makeChannel(WorkerHandle worker, std::size_t capacity, std::pmr::memory_resource* resource = nullptr) {
    auto* resolved = detail::pmrResourceOrDefault(resource);
    std::pmr::polymorphic_allocator<detail::ChannelState<T>> allocator(resolved);
    auto state = std::allocate_shared<detail::ChannelState<T>>(allocator, std::move(worker), capacity, resolved);
    detail::WorkerHandleAccess::registerShutdownListener(state->worker, state);
    return std::pair(ChannelSender<T>(state), ChannelReceiver<T>(state));
}

}  // namespace ruvia
