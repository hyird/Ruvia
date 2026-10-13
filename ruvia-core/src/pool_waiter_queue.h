#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>
#include <variant>

#include "ruvia/core/worker_wait_result.h"

namespace ruvia::detail {

struct pool_waiter_idle final {};
struct pool_waiter_queued final {};

using pool_waiter_result_type = worker_wait_result<std::size_t>;

// One coroutine waiting for a free connection slot in a per-worker connection
// pool. The node lives on the waiting coroutine's frame; the queue owns only
// the intrusive links, so enqueuing costs no allocation.
// pool_waiter is its own awaiter: the queue commits one typed result before it
// resumes the coroutine, so callers never coordinate external readiness flags.
class pool_waiter final {
public:
    explicit pool_waiter(std::uint64_t id) noexcept
        : id_(id) {}

    pool_waiter(const pool_waiter&) = delete;
    pool_waiter& operator=(const pool_waiter&) = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return std::holds_alternative<pool_waiter_result_type>(state_);
    }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
    }

    [[nodiscard]] const pool_waiter_result_type& await_resume() const noexcept {
        const auto* result_value = std::get_if<pool_waiter_result_type>(&state_);
        if (result_value == nullptr) {
            std::terminate();
        }
        return *result_value;
    }

private:
    friend class pool_waiter_queue;

    void complete(pool_waiter_result_type result_value) noexcept {
        if (!std::holds_alternative<pool_waiter_idle>(state_)) {
            std::terminate();
        }
        state_.template emplace<pool_waiter_result_type>(result_value);
    }

    void complete(worker_wait_status status) noexcept {
        complete(worker_wait_result_access::outcome<std::size_t>(status));
    }

    void resume() noexcept {
        auto handle = take_continuation();
        if (handle) {
            handle.resume();
        }
    }

    [[nodiscard]] std::coroutine_handle<> take_continuation() noexcept {
        return std::exchange(handle_, {});
    }

    using state_type = std::variant<pool_waiter_idle, pool_waiter_queued, pool_waiter_result_type>;

    state_type state_;
    std::uint64_t id_{0};
    std::coroutine_handle<> handle_{};
    pool_waiter* previous_{nullptr};
    pool_waiter* next_{nullptr};
};

// Intrusive FIFO shared by per-worker connection pools. Each pool is owned by a
// single worker io_context and only
// ever touched from that worker thread, so the queue needs no synchronization.
class pool_waiter_queue final {
public:
    [[nodiscard]] bool empty() const noexcept {
        return head_ == nullptr;
    }

    void enqueue(pool_waiter& waiter) noexcept {
        if (!std::holds_alternative<pool_waiter_idle>(waiter.state_)) {
            return;
        }
        waiter.previous_ = tail_;
        waiter.next_ = nullptr;
        waiter.state_.template emplace<pool_waiter_queued>();
        if (tail_ != nullptr) {
            tail_->next_ = &waiter;
        } else {
            head_ = &waiter;
        }
        tail_ = &waiter;
    }

    void remove(pool_waiter& waiter) noexcept {
        if (!std::holds_alternative<pool_waiter_queued>(waiter.state_)) {
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
        waiter.state_.template emplace<pool_waiter_idle>();
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

    [[nodiscard]] bool cancel(pool_waiter& waiter) noexcept {
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

    [[nodiscard]] bool expire(std::uint64_t id) noexcept {
        auto* waiter = find(id);
        if (waiter == nullptr || !std::holds_alternative<pool_waiter_queued>(waiter->state_)) {
            return false;
        }
        remove(*waiter);
        waiter->complete(worker_wait_status::timed_out);
        waiter->resume();
        return true;
    }

    // Commit closure for the entire current queue before resuming any waiter.
    // A resumed coroutine can re-enter the queue, so draining one waiter at a
    // time would let that continuation acquire a slot on behalf of a waiter
    // that should already have observed pool closure.
    void close_all() noexcept {
        pool_waiter* closed_head = nullptr;
        pool_waiter* closed_tail = nullptr;
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
        pool_waiter& waiter, std::coroutine_handle<>& continuation) noexcept {
        continuation = {};
        if (!std::holds_alternative<pool_waiter_queued>(waiter.state_)) {
            return false;
        }
        remove(waiter);
        waiter.complete(worker_wait_status::cancelled);
        continuation = waiter.take_continuation();
        return true;
    }

    [[nodiscard]] pool_waiter* find(std::uint64_t id) const noexcept {
        auto* waiter = head_;
        while (waiter != nullptr) {
            if (waiter->id_ == id) {
                return waiter;
            }
            waiter = waiter->next_;
        }
        return nullptr;
    }

    pool_waiter* head_{nullptr};
    pool_waiter* tail_{nullptr};
};

}  // namespace ruvia::detail
