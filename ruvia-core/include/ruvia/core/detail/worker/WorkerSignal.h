#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>

namespace ruvia::detail {

// Intrusive, allocation-free wake primitive for one worker. Every operation
// which touches its waiter list is worker-affine; the WorkerHandle is therefore
// the dispatch target and the affinity capability, not an optional fast path.
class WorkerSignal final {
    struct Awaiter;

public:
    explicit WorkerSignal(const WorkerHandle& worker)
        : worker_(requireWorker(worker)) {}
    WorkerSignal(WorkerHandle&&) = delete;

    ~WorkerSignal() {
        if (waiters_ != nullptr || scheduledWaiters_ != 0 || reservedWaits_ != 0) {
            std::terminate();
        }
    }

    WorkerSignal(const WorkerSignal&) = delete;
    WorkerSignal& operator=(const WorkerSignal&) = delete;

    [[nodiscard]] Task<void> wait() {
        requireCurrentWorker();
        return waitReserved(WaitReservation(*this));
    }

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

    void notify() noexcept;

private:
    enum class AwaitState : std::uint8_t {
        kIdle,
        kLinked,
        kScheduled,
    };

    [[nodiscard]] static const WorkerHandle& requireWorker(const WorkerHandle& worker) {
        if (!worker.valid()) {
            throw std::invalid_argument("worker signal requires a valid worker");
        }
        return worker;
    }

    void requireCurrentWorker() const {
        if (!worker_.isCurrent()) {
            throw std::logic_error("worker signal operation must run on its worker");
        }
    }

    void resumeScheduled(Awaiter* waiter, std::coroutine_handle<> continuation) noexcept;

    class WaitReservation final {
    public:
        explicit WaitReservation(WorkerSignal& signal) noexcept
            : signal_(&signal) {
            ++signal_->reservedWaits_;
        }
        ~WaitReservation() {
            if (signal_ != nullptr) {
                --signal_->reservedWaits_;
            }
        }

        WaitReservation(const WaitReservation&) = delete;
        WaitReservation& operator=(const WaitReservation&) = delete;
        WaitReservation(WaitReservation&& other) noexcept
            : signal_(std::exchange(other.signal_, nullptr)) {}
        WaitReservation& operator=(WaitReservation&&) = delete;

        [[nodiscard]] WorkerSignal& signal() const noexcept {
            return *signal_;
        }

    private:
        WorkerSignal* signal_;
    };

    [[nodiscard]] static Task<void> waitReserved(WaitReservation reservation) {
        auto& signal = reservation.signal();
        // wait() returns a lazy Task. Recheck affinity when that Task actually
        // starts: a cold wait can otherwise be created on the owner worker and
        // later started on another worker, mutating the intrusive list there.
        signal.requireCurrentWorker();
        co_await Awaiter{signal};
    }

    struct Awaiter final {
        explicit Awaiter(WorkerSignal& owner) noexcept
            : signal(owner) {}

        ~Awaiter() {
            if (state != AwaitState::kIdle) {
                std::terminate();
            }
        }

        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;

        [[nodiscard]] bool await_ready() noexcept {
            if (!signal.pending_) {
                return false;
            }
            signal.pending_ = false;
            return true;
        }

        bool await_suspend(std::coroutine_handle<> value) noexcept {
            continuation = value;
            next = signal.waiters_;
            state = AwaitState::kLinked;
            signal.waiters_ = this;
            return true;
        }

        void await_resume() const noexcept {}

        WorkerSignal& signal;
        Awaiter* next{nullptr};
        std::coroutine_handle<> continuation{};
        AwaitState state{AwaitState::kIdle};
    };

    // The owning session/connection keeps its stable worker handle alive until
    // every signal waiter and scheduled resumption has joined.
    const WorkerHandle& worker_;
    Awaiter* waiters_{nullptr};
    std::size_t scheduledWaiters_{0};
    std::size_t reservedWaits_{0};
    bool pending_{false};
};

inline void WorkerSignal::notify() noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }

    auto* waiter = std::exchange(waiters_, nullptr);
    if (waiter == nullptr) {
        pending_ = true;
        return;
    }

    while (waiter != nullptr) {
        auto* next = waiter->next;
        const auto continuation = waiter->continuation;
        waiter->next = nullptr;
        waiter->state = AwaitState::kScheduled;
        ++scheduledWaiters_;
        // A detached intrusive node has no recoverable owner. Dispatch failure
        // is terminal instead of silently stranding the continuation.
        WorkerHandleAccess::deferOrTerminate(worker_, [this, waiter, continuation] { resumeScheduled(waiter, continuation); });
        waiter = next;
    }
}

inline void WorkerSignal::resumeScheduled(Awaiter* waiter, std::coroutine_handle<> continuation) noexcept {
    if (!worker_.isCurrent() || waiter == nullptr || waiter->state != AwaitState::kScheduled || waiter->continuation != continuation || scheduledWaiters_ == 0) {
        std::terminate();
    }
    --scheduledWaiters_;
    waiter->continuation = {};
    waiter->state = AwaitState::kIdle;
    continuation.resume();
}

}  // namespace ruvia::detail
