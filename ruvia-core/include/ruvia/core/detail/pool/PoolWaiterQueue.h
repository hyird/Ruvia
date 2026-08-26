#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>
#include <variant>

namespace ruvia::detail {

class PoolLeaseScheduler;
class PoolWaiterResult;

class PoolWaiterAcquired final {
public:
    [[nodiscard]] constexpr std::size_t index() const noexcept {
        return index_;
    }

private:
    friend class PoolWaiterResult;

    explicit constexpr PoolWaiterAcquired(std::size_t index) noexcept
        : index_(index) {}

    std::size_t index_;
};

class PoolWaiterTimedOut final {
private:
    friend class PoolWaiterResult;

    constexpr PoolWaiterTimedOut() noexcept = default;
};

class PoolWaiterClosed final {
private:
    friend class PoolWaiterResult;

    constexpr PoolWaiterClosed() noexcept = default;
};

class PoolWaiterCancelled final {
private:
    friend class PoolWaiterResult;

    constexpr PoolWaiterCancelled() noexcept = default;
};

// A completed pool wait owns exactly one outcome. Only acquisition carries a
// slot index; timeout and pool closure can never expose a plausible sentinel.
class PoolWaiterResult final {
public:
    [[nodiscard]] constexpr const PoolWaiterAcquired* acquired() const& noexcept {
        return std::get_if<PoolWaiterAcquired>(&value_);
    }
    [[nodiscard]] constexpr const PoolWaiterAcquired* acquired() const&& = delete;

    [[nodiscard]] constexpr const PoolWaiterTimedOut* timedOut() const& noexcept {
        return std::get_if<PoolWaiterTimedOut>(&value_);
    }
    [[nodiscard]] constexpr const PoolWaiterTimedOut* timedOut() const&& = delete;

    [[nodiscard]] constexpr const PoolWaiterClosed* closed() const& noexcept {
        return std::get_if<PoolWaiterClosed>(&value_);
    }
    [[nodiscard]] constexpr const PoolWaiterClosed* closed() const&& = delete;

    [[nodiscard]] constexpr const PoolWaiterCancelled* cancelled() const& noexcept {
        return std::get_if<PoolWaiterCancelled>(&value_);
    }
    [[nodiscard]] constexpr const PoolWaiterCancelled* cancelled() const&& = delete;

private:
    friend class PoolWaiter;
    friend class PoolLeaseScheduler;

    using Value =
        std::variant<PoolWaiterAcquired, PoolWaiterTimedOut, PoolWaiterClosed, PoolWaiterCancelled>;

    template <typename Alternative>
    explicit constexpr PoolWaiterResult(Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    [[nodiscard]] static constexpr PoolWaiterResult makeAcquired(std::size_t index) noexcept {
        return PoolWaiterResult(PoolWaiterAcquired(index));
    }

    [[nodiscard]] static constexpr PoolWaiterResult makeTimedOut() noexcept {
        return PoolWaiterResult(PoolWaiterTimedOut());
    }

    [[nodiscard]] static constexpr PoolWaiterResult makeClosed() noexcept {
        return PoolWaiterResult(PoolWaiterClosed());
    }

    [[nodiscard]] static constexpr PoolWaiterResult makeCancelled() noexcept {
        return PoolWaiterResult(PoolWaiterCancelled());
    }

    Value value_;
};

struct PoolWaiterIdle final {};
struct PoolWaiterQueued final {};

// One coroutine waiting for a free connection slot in a per-worker connection
// pool. The node lives on the waiting coroutine's frame; the queue owns only
// the intrusive links, so enqueuing costs no allocation.
// PoolWaiter is its own awaiter: the queue commits one typed result before it
// resumes the coroutine, so callers never coordinate external readiness flags.
class PoolWaiter final {
public:
    explicit PoolWaiter(
        std::chrono::steady_clock::time_point deadline, std::uint64_t id = 0) noexcept
        : deadline_(deadline),
          id_(id) {}

    PoolWaiter(const PoolWaiter&) = delete;
    PoolWaiter& operator=(const PoolWaiter&) = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return std::holds_alternative<PoolWaiterResult>(state_);
    }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
    }

    [[nodiscard]] const PoolWaiterResult& await_resume() const noexcept {
        const auto* result = std::get_if<PoolWaiterResult>(&state_);
        if (result == nullptr) {
            std::terminate();
        }
        return *result;
    }

private:
    friend class PoolWaiterQueue;

    void complete(PoolWaiterResult result) noexcept {
        if (!std::holds_alternative<PoolWaiterIdle>(state_)) {
            std::terminate();
        }
        state_.template emplace<PoolWaiterResult>(std::move(result));
    }

    void completeAcquired(std::size_t index) noexcept {
        complete(PoolWaiterResult::makeAcquired(index));
    }

    void completeTimedOut() noexcept {
        complete(PoolWaiterResult::makeTimedOut());
    }

    void completeClosed() noexcept {
        complete(PoolWaiterResult::makeClosed());
    }

    void completeCancelled() noexcept {
        complete(PoolWaiterResult::makeCancelled());
    }

    void resume() noexcept {
        auto handle = takeContinuation();
        if (handle) {
            handle.resume();
        }
    }

    [[nodiscard]] std::coroutine_handle<> takeContinuation() noexcept {
        return std::exchange(handle_, {});
    }

    using State = std::variant<PoolWaiterIdle, PoolWaiterQueued, PoolWaiterResult>;

    State state_;
    std::chrono::steady_clock::time_point deadline_{};
    std::uint64_t id_{0};
    std::coroutine_handle<> handle_{};
    PoolWaiter* previous_{nullptr};
    PoolWaiter* next_{nullptr};
};

// Intrusive FIFO shared by per-worker connection pools. Each pool is owned by a
// single worker io_context and only
// ever touched from that worker thread, so the queue needs no synchronization.
class PoolWaiterQueue final {
public:
    [[nodiscard]] bool empty() const noexcept {
        return head_ == nullptr;
    }

    void enqueue(PoolWaiter& waiter) noexcept {
        if (!std::holds_alternative<PoolWaiterIdle>(waiter.state_)) {
            return;
        }
        waiter.previous_ = tail_;
        waiter.next_ = nullptr;
        waiter.state_.template emplace<PoolWaiterQueued>();
        if (tail_ != nullptr) {
            tail_->next_ = &waiter;
        } else {
            head_ = &waiter;
        }
        tail_ = &waiter;
    }

    void remove(PoolWaiter& waiter) noexcept {
        if (!std::holds_alternative<PoolWaiterQueued>(waiter.state_)) {
            return;
        }
        if (waiter.previous_ != nullptr) {
            waiter.previous_->next_ = waiter.next_;
        } else {
            head_ = waiter.next_;
        }
        if (waiter.next_ != nullptr) {
            waiter.next_->previous_ = waiter.previous_;
        } else {
            tail_ = waiter.previous_;
        }
        waiter.previous_ = nullptr;
        waiter.next_ = nullptr;
        waiter.state_.template emplace<PoolWaiterIdle>();
    }

    // Hand the freed slot `index` to the next waiter and resume it. Returns true
    // if a waiter took the slot, false if the queue was empty.
    [[nodiscard]] bool resumeNext(std::size_t index) noexcept {
        if (head_ == nullptr) {
            return false;
        }
        auto* waiter = head_;
        remove(*waiter);
        waiter->completeAcquired(index);
        waiter->resume();
        return true;
    }

    [[nodiscard]] bool cancel(PoolWaiter& waiter) noexcept {
        std::coroutine_handle<> continuation;
        if (!commitCancellation(waiter, continuation)) {
            return false;
        }
        if (continuation) {
            continuation.resume();
        }
        return true;
    }

    [[nodiscard]] bool cancel(std::uint64_t id) noexcept {
        auto* waiter = find(id);
        return waiter != nullptr && cancel(*waiter);
    }

    // Commit cancellation while still inside a stop callback, but let the
    // caller resume the coroutine only after that callback has returned.
    [[nodiscard]] bool commitCancellation(
        std::uint64_t id, std::coroutine_handle<>& continuation) noexcept {
        continuation = {};
        auto* waiter = find(id);
        return waiter != nullptr && commitCancellation(*waiter, continuation);
    }

    [[nodiscard]] bool expire(std::uint64_t id) noexcept {
        auto* waiter = find(id);
        if (waiter == nullptr || !std::holds_alternative<PoolWaiterQueued>(waiter->state_)) {
            return false;
        }
        remove(*waiter);
        waiter->completeTimedOut();
        waiter->resume();
        return true;
    }

    // Commit closure for the entire current queue before resuming any waiter.
    // A resumed coroutine can re-enter the queue, so draining one waiter at a
    // time would let that continuation acquire a slot on behalf of a waiter
    // that should already have observed pool closure.
    void closeAll() noexcept {
        PoolWaiter* closedHead = nullptr;
        PoolWaiter* closedTail = nullptr;
        while (head_ != nullptr) {
            auto* waiter = head_;
            remove(*waiter);
            waiter->completeClosed();
            if (closedTail != nullptr) {
                closedTail->next_ = waiter;
            } else {
                closedHead = waiter;
            }
            closedTail = waiter;
        }
        while (closedHead != nullptr) {
            auto* resumeWaiter = closedHead;
            closedHead = closedHead->next_;
            resumeWaiter->next_ = nullptr;
            resumeWaiter->resume();
        }
    }

    // Fail every waiter whose acquire deadline has passed. Callers must only
    // invoke this when an acquire timeout is configured; otherwise the deadlines
    // are not meaningful.
    void expireDeadlines(std::chrono::steady_clock::time_point now) noexcept {
        // Two-phase to stay safe against re-entrancy: resuming a timed-out waiter
        // can run application continuations that re-enter the pool (e.g. an outer
        // ConnectionGuard releasing during unwind, which resumeNext()s another
        // queued waiter). So first detach every expired waiter into a private
        // list, then resume them once the traversal is complete — a resumed
        // coroutine can no longer touch a node that is already off the queue, so
        // no waiter is ever resumed twice. closeAll() applies the same
        // commit-before-resume rule to the entire queue.
        PoolWaiter* expiredHead = nullptr;
        PoolWaiter* expiredTail = nullptr;
        auto* waiter = head_;
        while (waiter != nullptr) {
            auto* next = waiter->next_;
            if (waiter->deadline_ <= now) {
                remove(*waiter);
                waiter->completeTimedOut();
                waiter->next_ = nullptr;
                if (expiredTail != nullptr) {
                    expiredTail->next_ = waiter;
                } else {
                    expiredHead = waiter;
                }
                expiredTail = waiter;
            }
            waiter = next;
        }
        while (expiredHead != nullptr) {
            auto* resumeWaiter = expiredHead;
            expiredHead = expiredHead->next_;
            resumeWaiter->resume();
        }
    }

private:
    [[nodiscard]] bool commitCancellation(
        PoolWaiter& waiter, std::coroutine_handle<>& continuation) noexcept {
        continuation = {};
        if (!std::holds_alternative<PoolWaiterQueued>(waiter.state_)) {
            return false;
        }
        remove(waiter);
        waiter.completeCancelled();
        continuation = waiter.takeContinuation();
        return true;
    }

    [[nodiscard]] PoolWaiter* find(std::uint64_t id) const noexcept {
        auto* waiter = head_;
        while (waiter != nullptr) {
            if (waiter->id_ == id) {
                return waiter;
            }
            waiter = waiter->next_;
        }
        return nullptr;
    }

    PoolWaiter* head_{nullptr};
    PoolWaiter* tail_{nullptr};
};

}  // namespace ruvia::detail
