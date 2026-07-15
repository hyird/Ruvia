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
#include <utility>
#include <variant>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>
#include <ruvia/core/WorkerWaitResult.h>
#include <ruvia/core/detail/WorkerDispatcher.h>
#include <ruvia/core/detail/WorkerTimer.h>
#include <ruvia/core/detail/WorkerWaitAwaiter.h>
#include <ruvia/core/memory/PmrResource.h>

namespace ruvia {

enum class ChannelSendResult : std::uint8_t {
    kSent,
    kFull,
    kClosed,
    kWorkerStopping,
};

template <typename T>
class ChannelSender;

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
    ChannelState(WorkerHandle target,
                 std::size_t requestedCapacity,
                 std::pmr::memory_resource* resource)
        : worker(std::move(target)), slots(resource) {
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
    using Lifecycle = std::variant<
        ChannelOpen,
        ChannelClosed,
        ChannelWorkerStopping>;
    Lifecycle lifecycle;
    ChannelReceiveAwaiter<T>* waiter{nullptr};

    void workerStopping() noexcept override;
};

template <typename T>
struct ChannelReceiveAwaiter final {
    ChannelReceiveAwaiter(
        std::shared_ptr<ChannelState<T>> value,
        std::optional<std::chrono::steady_clock::duration> timeoutValue = std::nullopt)
        : state(std::move(value)), timeout(timeoutValue) {}

    [[nodiscard]] bool await_ready() {
        std::lock_guard lock(state->mutex);
        if (state->size != 0) {
            (void)completion.complete(WorkerWaitResultAccess::value(
                std::move(*state->slots[state->head])));
            state->slots[state->head].reset();
            state->head = (state->head + 1) % state->slots.size();
            --state->size;
            return true;
        }
        if (std::holds_alternative<ChannelWorkerStopping>(state->lifecycle)) {
            (void)completion.complete(
                WorkerWaitResultAccess::workerStopping<T>());
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
        std::lock_guard lock(state->mutex);
        if (!completion.suspend(handle)) {
            return false;
        }
        if (timeout) {
            try {
                timer = WorkerHandleAccess::scheduleTimer(
                    state->worker, std::chrono::steady_clock::now() + *timeout,
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

    std::shared_ptr<ChannelState<T>> state;
    std::optional<std::chrono::steady_clock::duration> timeout;
    WorkerTimerRegistration timer;
    WorkerWaitAwaitState<T> completion;
};

template <typename T>
[[nodiscard]] Task<WorkerWaitResult<T>> receiveChannelState(
    std::shared_ptr<ChannelState<T>> state,
    std::optional<std::chrono::steady_clock::duration> timeout) {
    if (!state || !state->worker.isCurrent()) {
        throw std::logic_error("channel receive must run on its bound worker");
    }
    co_return co_await ChannelReceiveAwaiter<T>(std::move(state), timeout);
}

template <typename T>
void wakeChannelReceiver(ChannelReceiveAwaiter<T>* waiter) {
    if (waiter->timer.valid()) {
        waiter->timer.cancel();
        return;
    }
    WorkerHandleAccess::defer(
        waiter->state->worker,
        [waiter] { waiter->completion.continuation().resume(); });
}

template <typename T>
void ChannelState<T>::workerStopping() noexcept {
    ChannelReceiveAwaiter<T>* pending = nullptr;
    bool wake = false;
    {
        std::lock_guard lock(mutex);
        lifecycle.template emplace<ChannelWorkerStopping>();
        pending = std::exchange(waiter, nullptr);
        if (pending != nullptr) {
            wake = pending->completion.complete(
                WorkerWaitResultAccess::workerStopping<T>());
        }
    }
    if (wake) {
        try {
            wakeChannelReceiver(pending);
        } catch (...) {
            std::terminate();
        }
    }
}

}

template <typename T>
class ChannelSender final {
public:
    ChannelSender() noexcept = default;

    [[nodiscard]] ChannelSendResult send(T value) const {
        if (!state_) {
            return ChannelSendResult::kClosed;
        }
        detail::ChannelReceiveAwaiter<T>* waiter = nullptr;
        bool wake = false;
        {
            std::lock_guard lock(state_->mutex);
            if (std::holds_alternative<detail::ChannelClosed>(
                    state_->lifecycle)) {
                return ChannelSendResult::kClosed;
            }
            if (std::holds_alternative<detail::ChannelWorkerStopping>(
                    state_->lifecycle)) {
                return ChannelSendResult::kWorkerStopping;
            }
            assert(std::holds_alternative<detail::ChannelOpen>(state_->lifecycle));
            if (!state_->worker.accepting()) {
                return ChannelSendResult::kWorkerStopping;
            }
            if (state_->waiter != nullptr) {
                waiter = state_->waiter;
                wake = waiter->completion.complete(
                    detail::WorkerWaitResultAccess::value(std::move(value)));
                state_->waiter = nullptr;
            } else {
                if (state_->size == state_->slots.size()) {
                    return ChannelSendResult::kFull;
                }
                state_->slots[state_->tail].emplace(std::move(value));
                state_->tail = (state_->tail + 1) % state_->slots.size();
                ++state_->size;
            }
        }
        if (wake) {
            detail::wakeChannelReceiver(waiter);
        }
        return ChannelSendResult::kSent;
    }

    void close() const {
        if (!state_) {
            return;
        }
        detail::ChannelReceiveAwaiter<T>* waiter = nullptr;
        bool wake = false;
        {
            std::lock_guard lock(state_->mutex);
            if (!std::holds_alternative<detail::ChannelOpen>(
                    state_->lifecycle)) {
                return;
            }
            state_->lifecycle.template emplace<detail::ChannelClosed>();
            waiter = std::exchange(state_->waiter, nullptr);
            if (waiter != nullptr) {
                wake = waiter->completion.complete(
                    detail::WorkerWaitResultAccess::closed<T>());
            }
        }
        if (wake) {
            detail::wakeChannelReceiver(waiter);
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

    ~ChannelReceiver() { close(); }

    [[nodiscard]] Task<WorkerWaitResult<T>> receive() {
        return detail::receiveChannelState<T>(state_, std::nullopt);
    }

    template <typename Rep, typename Period>
    [[nodiscard]] Task<WorkerWaitResult<T>>
    receiveFor(std::chrono::duration<Rep, Period> duration) {
        return detail::receiveChannelState<T>(
            state_,
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                duration));
    }

    void close() const { ChannelSender<T>(state_).close(); }

private:
    explicit ChannelReceiver(std::shared_ptr<detail::ChannelState<T>> state)
        : state_(std::move(state)) {}

    std::shared_ptr<detail::ChannelState<T>> state_;
    template <typename U>
    friend auto makeChannel(WorkerHandle, std::size_t, std::pmr::memory_resource*);
};

template <typename T>
[[nodiscard]] auto makeChannel(WorkerHandle worker,
                               std::size_t capacity,
                               std::pmr::memory_resource* resource = nullptr) {
    auto* resolved = detail::pmrResourceOrDefault(resource);
    std::pmr::polymorphic_allocator<detail::ChannelState<T>> allocator(resolved);
    auto state = std::allocate_shared<detail::ChannelState<T>>(
        allocator, std::move(worker), capacity, resolved);
    detail::WorkerHandleAccess::registerShutdownListener(state->worker, state);
    return std::pair(ChannelSender<T>(state), ChannelReceiver<T>(state));
}

}
