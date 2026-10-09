#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>
#include <variant>

namespace ruvia::detail {

class pool_lease_scheduler;
class pool_waiter_result;

class pool_waiter_acquired final {
public:
    [[nodiscard]] constexpr std::size_t index() const noexcept {
        return index_;
    }

private:
    friend class pool_waiter_result;

    explicit constexpr pool_waiter_acquired(std::size_t index) noexcept
        : index_(index) {}

    std::size_t index_;
};

class pool_waiter_timed_out final {
private:
    friend class pool_waiter_result;

    constexpr pool_waiter_timed_out() noexcept = default;
};

class pool_waiter_closed final {
private:
    friend class pool_waiter_result;

    constexpr pool_waiter_closed() noexcept = default;
};

class pool_waiter_cancelled final {
private:
    friend class pool_waiter_result;

    constexpr pool_waiter_cancelled() noexcept = default;
};

// A completed pool wait owns exactly one outcome. Only acquisition carries a
// slot index; timeout and pool closure can never expose a plausible sentinel.
class pool_waiter_result final {
public:
    [[nodiscard]] constexpr const pool_waiter_acquired* acquired() const& noexcept {
        return std::get_if<pool_waiter_acquired>(&value_);
    }
    [[nodiscard]] constexpr const pool_waiter_acquired* acquired() const&& = delete;

    [[nodiscard]] constexpr const pool_waiter_timed_out* timed_out() const& noexcept {
        return std::get_if<pool_waiter_timed_out>(&value_);
    }
    [[nodiscard]] constexpr const pool_waiter_timed_out* timed_out() const&& = delete;

    [[nodiscard]] constexpr const pool_waiter_closed* closed() const& noexcept {
        return std::get_if<pool_waiter_closed>(&value_);
    }
    [[nodiscard]] constexpr const pool_waiter_closed* closed() const&& = delete;

    [[nodiscard]] constexpr const pool_waiter_cancelled* cancelled() const& noexcept {
        return std::get_if<pool_waiter_cancelled>(&value_);
    }
    [[nodiscard]] constexpr const pool_waiter_cancelled* cancelled() const&& = delete;

private:
    friend class pool_waiter;
    friend class pool_lease_scheduler;

    using value_type =
        std::variant<pool_waiter_acquired, pool_waiter_timed_out, pool_waiter_closed, pool_waiter_cancelled>;

    template <typename alternative_type>
    explicit constexpr pool_waiter_result(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    [[nodiscard]] static constexpr pool_waiter_result make_acquired(std::size_t index) noexcept {
        return pool_waiter_result(pool_waiter_acquired(index));
    }

    [[nodiscard]] static constexpr pool_waiter_result make_timed_out() noexcept {
        return pool_waiter_result(pool_waiter_timed_out());
    }

    [[nodiscard]] static constexpr pool_waiter_result make_closed() noexcept {
        return pool_waiter_result(pool_waiter_closed());
    }

    [[nodiscard]] static constexpr pool_waiter_result make_cancelled() noexcept {
        return pool_waiter_result(pool_waiter_cancelled());
    }

    value_type value_;
};

struct pool_waiter_idle final {};
struct pool_waiter_queued final {};

// One coroutine waiting for a free connection slot in a per-worker connection
// pool. The node lives on the waiting coroutine's frame; the queue owns only
// the intrusive links, so enqueuing costs no allocation.
// pool_waiter is its own awaiter: the queue commits one typed result before it
// resumes the coroutine, so callers never coordinate external readiness flags.
class pool_waiter final {
public:
    explicit pool_waiter(
        std::chrono::steady_clock::time_point deadline_value, std::uint64_t id = 0) noexcept
        : deadline_(deadline_value),
          id_(id) {}

    pool_waiter(const pool_waiter&) = delete;
    pool_waiter& operator=(const pool_waiter&) = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return std::holds_alternative<pool_waiter_result>(state_);
    }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
    }

    [[nodiscard]] const pool_waiter_result& await_resume() const noexcept {
        const auto* result_value = std::get_if<pool_waiter_result>(&state_);
        if (result_value == nullptr) {
            std::terminate();
        }
        return *result_value;
    }

private:
    friend class pool_waiter_queue;

    void complete(pool_waiter_result result_value) noexcept {
        if (!std::holds_alternative<pool_waiter_idle>(state_)) {
            std::terminate();
        }
        state_.template emplace<pool_waiter_result>(result_value);
    }

    void complete_acquired(std::size_t index) noexcept {
        complete(pool_waiter_result::make_acquired(index));
    }

    void complete_timed_out() noexcept {
        complete(pool_waiter_result::make_timed_out());
    }

    void complete_closed() noexcept {
        complete(pool_waiter_result::make_closed());
    }

    void complete_cancelled() noexcept {
        complete(pool_waiter_result::make_cancelled());
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

    using state_type = std::variant<pool_waiter_idle, pool_waiter_queued, pool_waiter_result>;

    state_type state_;
    std::chrono::steady_clock::time_point deadline_{};
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
        waiter->complete_acquired(index);
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
        waiter->complete_timed_out();
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
            waiter->complete_closed();
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

    // Fail every waiter whose acquire deadline has passed. Callers must only
    // invoke this when an acquire timeout is configured; otherwise the deadlines
    // are not meaningful.
    void expire_deadlines(std::chrono::steady_clock::time_point now) noexcept {
        // Two-phase to stay safe against re-entrancy: resuming a timed-out waiter
        // can run application continuations that re-enter the pool (e.g. an outer
        // connection lease releasing during unwind, which resumes another
        // queued waiter). So first detach every expired waiter into a private
        // list, then resume them once the traversal is complete — a resumed
        // coroutine can no longer touch a node that is already off the queue, so
        // no waiter is ever resumed twice. close_all() applies the same
        // commit-before-resume rule to the entire queue.
        pool_waiter* expired_head = nullptr;
        pool_waiter* expired_tail = nullptr;
        auto* waiter = head_;
        while (waiter != nullptr) {
            auto* next_value = waiter->next_;
            if (waiter->deadline_ <= now) {
                remove(*waiter);
                waiter->complete_timed_out();
                waiter->next_ = nullptr;
                if (expired_tail != nullptr) {
                    expired_tail->next_ = waiter;
                } else {
                    expired_head = waiter;
                }
                expired_tail = waiter;
            }
            waiter = next_value;
        }
        while (expired_head != nullptr) {
            auto* resume_waiter = expired_head;
            expired_head = expired_head->next_;
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
        waiter.complete_cancelled();
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
