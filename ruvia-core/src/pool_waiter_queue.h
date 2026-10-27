#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "ruvia/core/pool_lease_scheduler.h"
#include "ruvia/core/worker_wait_result.h"

namespace ruvia::detail {

// Intrusive FIFO of one pool_lease_scheduler. The scheduler is only ever
// touched from its worker thread, so the queue needs no synchronization.
class pool_waiter_queue final {
public:
    void enqueue(pool_lease_acquire& waiter) noexcept {
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

    void remove(pool_lease_acquire& waiter) noexcept {
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
    [[nodiscard]] bool resume_next(std::size_t index) noexcept {
        if (head_ == nullptr) {
            return false;
        }
        auto* waiter = head_;
        remove(*waiter);
        waiter->complete(worker_wait_result_access::value(index));
        waiter->resume();
        return true;
    }

    [[nodiscard]] bool cancel(pool_lease_acquire& waiter) noexcept {
        std::coroutine_handle<> continuation;
        if (!commit_cancellation(waiter, continuation)) {
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
    [[nodiscard]] bool commit_cancellation(
        std::uint64_t id, std::coroutine_handle<>& continuation) noexcept {
        continuation = {};
        auto* waiter = find(id);
        return waiter != nullptr && commit_cancellation(*waiter, continuation);
    }

    [[nodiscard]] bool expire(pool_lease_acquire& waiter) noexcept {
        if (!waiter.queued_) {
            return false;
        }
        remove(waiter);
        waiter.complete(worker_wait_status::timed_out);
        waiter.resume();
        return true;
    }

    // Commit closure for the entire current queue before resuming any waiter.
    // A resumed coroutine can re-enter the queue, so draining one waiter at a
    // time would let that continuation acquire a slot on behalf of a waiter
    // that should already have observed pool closure.
    void close_all() noexcept {
        pool_lease_acquire* closed_head = nullptr;
        pool_lease_acquire* closed_tail = nullptr;
        while (head_ != nullptr) {
            auto* waiter = head_;
            remove(*waiter);
            waiter->complete(worker_wait_status::closed);
            if (closed_tail != nullptr) {
                closed_tail->next_ = waiter;
            } else {
                closed_head = waiter;
            }
            closed_tail = waiter;
        }
        while (closed_head != nullptr) {
            auto* resume_waiter = closed_head;
            closed_head = closed_head->next_;
            resume_waiter->next_ = nullptr;
            resume_waiter->resume();
        }
    }

private:
    [[nodiscard]] bool commit_cancellation(
        pool_lease_acquire& waiter, std::coroutine_handle<>& continuation) noexcept {
        continuation = {};
        if (!waiter.queued_) {
            return false;
        }
        remove(waiter);
        waiter.complete(worker_wait_status::cancelled);
        continuation = waiter.take_continuation();
        return true;
    }

    [[nodiscard]] pool_lease_acquire* find(std::uint64_t id) const noexcept {
        auto* waiter = head_;
        while (waiter != nullptr) {
            if (waiter->waiter_id_ == id) {
                return waiter;
            }
            waiter = waiter->next_;
        }
        return nullptr;
    }

    pool_lease_acquire* head_{nullptr};
    pool_lease_acquire* tail_{nullptr};
};

}  // namespace ruvia::detail
