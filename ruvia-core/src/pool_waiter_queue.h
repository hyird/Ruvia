#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "ruvia/core/pool_lease_scheduler.h"
#include "ruvia/core/worker_handle.h"
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

    // Commit the freed slot to the next waiter before deferring its continuation.
    // Cancellation and closure cannot replace this result after the handoff.
    [[nodiscard]] bool commit_handoff(
        std::size_t index, std::coroutine_handle<>& continuation) noexcept {
        continuation = {};
        if (head_ == nullptr) {
            return false;
        }
        auto* waiter = head_;
        remove(*waiter);
        waiter->complete(worker_wait_result_access::value(index));
        waiter->cancel_registrations();
        continuation = waiter->take_continuation();
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

    [[nodiscard]] bool expire(pool_lease_acquire& waiter) noexcept {
        if (!waiter.queued_) {
            return false;
        }
        remove(waiter);
        waiter.complete(worker_wait_status::timed_out);
        waiter.resume();
        return true;
    }

    // Commit closure for the entire current queue and resume every waiter in a
    // later worker turn, as release() does: closure runs inside the owner's own
    // teardown, which must finish before any waiter observes it.
    void close_all(const worker_handle& worker) noexcept {
        while (head_ != nullptr) {
            auto* waiter = head_;
            remove(*waiter);
            waiter->complete(worker_wait_status::closed);
            waiter->cancel_registrations();
            if (auto continuation = waiter->take_continuation()) {
                worker.defer_resume(continuation);
            }
        }
    }

private:
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
