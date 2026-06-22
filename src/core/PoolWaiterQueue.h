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
struct PoolWaiter {
    bool* ready{nullptr};
    bool* timedOut{nullptr};
    std::size_t* index{nullptr};
    std::chrono::steady_clock::time_point deadline{};
    std::coroutine_handle<> handle{};
    PoolWaiter* previous{nullptr};
    PoolWaiter* next{nullptr};
    bool queued{false};
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
        if (waiter.queued) {
            return;
        }
        waiter.previous = tail_;
        waiter.next = nullptr;
        waiter.queued = true;
        if (tail_ != nullptr) {
            tail_->next = &waiter;
        } else {
            head_ = &waiter;
        }
        tail_ = &waiter;
    }

    void remove(PoolWaiter& waiter) noexcept {
        if (!waiter.queued) {
            return;
        }
        if (waiter.previous != nullptr) {
            waiter.previous->next = waiter.next;
        } else {
            head_ = waiter.next;
        }
        if (waiter.next != nullptr) {
            waiter.next->previous = waiter.previous;
        } else {
            tail_ = waiter.previous;
        }
        waiter.previous = nullptr;
        waiter.next = nullptr;
        waiter.queued = false;
    }

    // Hand the freed slot `index` to the next waiter and resume it. Returns true
    // if a waiter took the slot, false if the queue was empty.
    [[nodiscard]] bool resumeNext(std::size_t index) noexcept {
        while (head_ != nullptr) {
            auto* waiter = head_;
            remove(*waiter);
            if (waiter->ready != nullptr && waiter->index != nullptr) {
                *waiter->index = index;
                *waiter->ready = true;
                if (waiter->handle) {
                    waiter->handle.resume();
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
            if (waiter->timedOut != nullptr) {
                *waiter->timedOut = false;
            }
            if (waiter->index != nullptr) {
                *waiter->index = sentinelIndex;
            }
            if (waiter->ready != nullptr) {
                *waiter->ready = true;
            }
            if (waiter->handle) {
                waiter->handle.resume();
            }
        }
    }

    // Fail every waiter whose acquire deadline has passed. Callers must only
    // invoke this when an acquire timeout is configured; otherwise the deadlines
    // are not meaningful.
    void expireDeadlines(std::chrono::steady_clock::time_point now) noexcept {
        auto* waiter = head_;
        while (waiter != nullptr) {
            auto* next = waiter->next;
            if (waiter->deadline <= now) {
                remove(*waiter);
                if (waiter->timedOut != nullptr) {
                    *waiter->timedOut = true;
                }
                if (waiter->ready != nullptr) {
                    *waiter->ready = true;
                }
                if (waiter->handle) {
                    waiter->handle.resume();
                }
            }
            waiter = next;
        }
    }

private:
    PoolWaiter* head_{nullptr};
    PoolWaiter* tail_{nullptr};
};

}  // namespace ruvia::detail
