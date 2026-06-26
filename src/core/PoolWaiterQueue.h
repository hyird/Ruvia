#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>

namespace ruvia::detail {

// One coroutine waiting for a free connection slot in a per-worker connection
// pool. The node lives on the waiting coroutine's frame; the queue owns only
// the intrusive links, so enqueuing costs no allocation.
//
// `index` receives the slot handed to the waiter, `ready`/`timedOut` are the
// awaiter's resume flags, all owned by the suspended coroutine.
class PoolWaiter final {
public:
    PoolWaiter() noexcept = default;
    PoolWaiter(
        bool& ready,
        bool& timedOut,
        std::size_t& index,
        std::chrono::steady_clock::time_point deadline) noexcept {
        bind(ready, timedOut, index, deadline, {});
    }

    PoolWaiter(const PoolWaiter&) = delete;
    PoolWaiter& operator=(const PoolWaiter&) = delete;

    void bind(
        bool& ready,
        bool& timedOut,
        std::size_t& index,
        std::chrono::steady_clock::time_point deadline,
        std::coroutine_handle<> handle) noexcept {
        ready_ = &ready;
        timedOut_ = &timedOut;
        index_ = &index;
        deadline_ = deadline;
        handle_ = handle;
    }

    void setHandle(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
    }

private:
    friend class PoolWaiterQueue;

    bool* ready_{nullptr};
    bool* timedOut_{nullptr};
    std::size_t* index_{nullptr};
    std::chrono::steady_clock::time_point deadline_{};
    std::coroutine_handle<> handle_{};
    PoolWaiter* previous_{nullptr};
    PoolWaiter* next_{nullptr};
    bool queued_{false};
};

// Intrusive FIFO of pool acquire-waiters shared by the DB, Redis and HTTP-client
// connection pools. Each pool is owned by a single worker io_context and only
// ever touched from that worker thread, so the queue needs no synchronization.
class PoolWaiterQueue final {
public:
    [[nodiscard]] bool empty() const noexcept {
        return head_ == nullptr;
    }

    void enqueue(PoolWaiter& waiter) noexcept {
        if (waiter.queued_) {
            return;
        }
        waiter.previous_ = tail_;
        waiter.next_ = nullptr;
        waiter.queued_ = true;
        if (tail_ != nullptr) {
            tail_->next_ = &waiter;
        } else {
            head_ = &waiter;
        }
        tail_ = &waiter;
    }

    void remove(PoolWaiter& waiter) noexcept {
        if (!waiter.queued_) {
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
        waiter.queued_ = false;
    }

    // Hand the freed slot `index` to the next waiter and resume it. Returns true
    // if a waiter took the slot, false if the queue was empty.
    [[nodiscard]] bool resumeNext(std::size_t index) noexcept {
        while (head_ != nullptr) {
            auto* waiter = head_;
            remove(*waiter);
            if (waiter->ready_ != nullptr && waiter->index_ != nullptr) {
                *waiter->index_ = index;
                *waiter->ready_ = true;
                if (waiter->handle_) {
                    waiter->handle_.resume();
                }
                return true;
            }
        }
        return false;
    }

    // Wake every waiter on pool shutdown with a sentinel slot so each acquire
    // observes the closing pool and fails (not as a timeout).
    void closeAll(std::size_t sentinelIndex) noexcept {
        while (head_ != nullptr) {
            auto* waiter = head_;
            remove(*waiter);
            if (waiter->timedOut_ != nullptr) {
                *waiter->timedOut_ = false;
            }
            if (waiter->index_ != nullptr) {
                *waiter->index_ = sentinelIndex;
            }
            if (waiter->ready_ != nullptr) {
                *waiter->ready_ = true;
            }
            if (waiter->handle_) {
                waiter->handle_.resume();
            }
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
        // no waiter is ever resumed twice. (closeAll() is re-entrancy-safe for
        // the same reason: it re-reads head_ after every resume.)
        PoolWaiter* expiredHead = nullptr;
        PoolWaiter* expiredTail = nullptr;
        auto* waiter = head_;
        while (waiter != nullptr) {
            auto* next = waiter->next_;
            if (waiter->deadline_ <= now) {
                remove(*waiter);
                if (waiter->timedOut_ != nullptr) {
                    *waiter->timedOut_ = true;
                }
                if (waiter->ready_ != nullptr) {
                    *waiter->ready_ = true;
                }
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
            if (resumeWaiter->handle_) {
                resumeWaiter->handle_.resume();
            }
        }
    }

private:
    PoolWaiter* head_{nullptr};
    PoolWaiter* tail_{nullptr};
};

}  // namespace ruvia::detail
