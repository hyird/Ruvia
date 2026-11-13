#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia {

// Intrusive, allocation-free wake primitive for one worker. Every operation
// which touches its waiter list is worker-affine; the worker_ref is therefore
// the dispatch target and the affinity capability, not an optional fast path.
class worker_signal final {
    enum class await_state_type : std::uint8_t {
        idle,
        linked,
        scheduled,
    };

public:
    class wait_operation final {
    public:
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        // Only a cold operation may move; a linked waiter must stay address-stable.
        wait_operation(wait_operation&& other) noexcept
            : signal_(std::exchange(other.signal_, nullptr)) {
            if (other.started_) {
                std::terminate();
            }
        }
        wait_operation& operator=(wait_operation&&) = delete;
        void operator&() const = delete;
        ~wait_operation() {
            if (state_ != await_state_type::idle) {
                std::terminate();
            }
            release_reservation();
        }

        [[nodiscard]] bool await_ready() {
            if (signal_ == nullptr || started_) {
                throw std::logic_error("worker signal wait cannot be awaited");
            }
            started_ = true;
            try {
                signal_->require_current_worker();
            } catch (...) {
                release_reservation();
                throw;
            }
            if (!signal_->pending_) {
                return false;
            }
            signal_->pending_ = false;
            return true;
        }
        bool await_suspend(std::coroutine_handle<> value) noexcept {
            continuation_ = value;
            next_ = signal_->waiters_;
            state_ = await_state_type::linked;
            signal_->waiters_ = this;
            return true;
        }
        void await_resume() noexcept {
            release_reservation();
        }

    private:
        explicit wait_operation(worker_signal& signal) noexcept
            : signal_(&signal) {
            ++signal.reserved_waits_;
        }
        void release_reservation() noexcept {
            if (signal_ != nullptr) {
                --signal_->reserved_waits_;
                signal_ = nullptr;
            }
        }
        worker_signal* signal_;
        wait_operation* next_{nullptr};
        std::coroutine_handle<> continuation_{};
        await_state_type state_{await_state_type::idle};
        bool started_{false};
        friend class worker_signal;
    };

private:
    using awaiter_type = wait_operation;

public:
    explicit worker_signal(const worker_ref& worker_value)
        : worker_(require_worker(worker_value)) {}
    worker_signal(worker_ref&&) = delete;
    worker_signal(const worker_ref&&) = delete;

    ~worker_signal() {
        if (waiters_ != nullptr || scheduled_waiters_ != 0 || reserved_waits_ != 0) {
            std::terminate();
        }
    }

    worker_signal(const worker_signal&) = delete;
    worker_signal& operator=(const worker_signal&) = delete;

    // A cold single-use awaiter reserves the signal until completion or destruction.
    // Creation and starting the wait both require the bound worker.
    [[nodiscard]] wait_operation wait() & {
        require_current_worker();
        return wait_operation(*this);
    }
    wait_operation wait() && = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept {
        return worker_;
    }
    const worker_ref& worker() const&& = delete;

    // Wakes every waiter, or leaves one pending wake when none waits. Like
    // wait(), it must run on the bound worker and throws std::logic_error
    // elsewhere. A failed resume dispatch after waiters were detached has no
    // recoverable owner and terminates.
    void notify();

private:
    [[nodiscard]] static const worker_ref& require_worker(const worker_ref& worker_value) {
        if (!worker_value.valid()) {
            throw std::invalid_argument("worker signal requires a valid worker");
        }
        return worker_value;
    }

    void require_current_worker() const {
        if (!worker_.is_current()) {
            throw std::logic_error("worker signal operation must run on its worker");
        }
    }

    void resume_scheduled(awaiter_type* waiter, std::coroutine_handle<> continuation) noexcept;

    // The owning session/connection keeps its stable worker_ref alive until
    // every signal waiter and scheduled resumption has joined.
    const worker_ref& worker_;
    awaiter_type* waiters_{nullptr};
    std::size_t scheduled_waiters_{0};
    std::size_t reserved_waits_{0};
    bool pending_{false};
};

inline void worker_signal::notify() {
    require_current_worker();

    auto* waiter = std::exchange(waiters_, nullptr);
    if (waiter == nullptr) {
        pending_ = true;
        return;
    }

    while (waiter != nullptr) {
        auto* next_value = waiter->next_;
        const auto continuation = waiter->continuation_;
        waiter->next_ = nullptr;
        waiter->state_ = await_state_type::scheduled;
        ++scheduled_waiters_;
        // A detached intrusive node has no recoverable owner. Dispatch failure
        // is terminal instead of silently stranding the continuation. On the
        // worker's own thread this small fixed callable is posted directly.
        detail::worker_ref_access::defer_or_terminate(
            worker_, [this, waiter, continuation] { resume_scheduled(waiter, continuation); });
        waiter = next_value;
    }
}

inline void worker_signal::resume_scheduled(
    awaiter_type* waiter, std::coroutine_handle<> continuation) noexcept {
    if (!worker_.is_current() || waiter->state_ != await_state_type::scheduled ||
        waiter->continuation_ != continuation || scheduled_waiters_ == 0) {
        std::terminate();
    }
    --scheduled_waiters_;
    waiter->continuation_ = {};
    waiter->state_ = await_state_type::idle;
    continuation.resume();
}

}  // namespace ruvia

namespace ruvia {

enum class timer_sleep_result : std::uint8_t {
    elapsed,
    // The sleep was cut short because a stop was requested: the worker is
    // shutting down, or -- when a stop_token was passed -- whatever else owns
    // that token asked the work to stop. Named for the request rather than
    // for one of its causes, because a caller reacts to it the same way either
    // way: give up the rest of the wait.
    stop_requested,
};

class sleep_operation;

// Suspend the current coroutine on its bound worker for `duration`, or until
// `deadline`. The result distinguishes a normal elapsed delay from
// cancellation caused by worker shutdown (stop_timers): a sleep pending at
// that point, or started after the worker's timer queue stopped, returns
// stop_requested without throwing. The duration is the sleep's target, not
// an operation bound (operation_timeout): sleep_for(d) is sleep_until(now +
// d), so a non-positive duration, like a deadline that has passed, elapses
// without suspending. Durations saturate, so an effectively unbounded delay such as
// hours::max() waits for stop instead of overflowing.
//
// The sleep is also cut short when `stop_token_value` is stopped. A
// framework-provided wait that ignores the caller's stop token is a hole in
// every deadline built on that token, so anything a request can await takes
// one. The stop callback may run on a thread that is not this worker, and
// cancelling a worker timer is worker-owned state, so cancellation is deferred
// onto the worker and validated there -- a stop that arrives after the sleep
// already finished is a no-op rather than a use-after-free.
//
// The worker is borrowed for the lifetime of the returned operation: the
// caller must keep its address-stable worker_ref alive until the operation
// completes or is dropped cold. This is the same borrow-only boundary the
// request hot path applies everywhere else; a temporary worker_ref would dangle in
// the cold operation.
[[nodiscard]] sleep_operation sleep_for(
    const worker_ref& worker_value, timeout_duration duration, stop_token stop_token_value = {});
sleep_operation sleep_for(worker_ref&&, timeout_duration, stop_token = {}) = delete;
sleep_operation sleep_for(const worker_ref&&, timeout_duration, stop_token = {}) = delete;
[[nodiscard]] sleep_operation sleep_until(const worker_ref& worker_value,
    std::chrono::steady_clock::time_point deadline, stop_token stop_token_value = {});
sleep_operation sleep_until(
    worker_ref&&, std::chrono::steady_clock::time_point, stop_token = {}) = delete;
sleep_operation sleep_until(
    const worker_ref&&, std::chrono::steady_clock::time_point, stop_token = {}) = delete;

// One sleep on the caller's worker: single-use, movable only before it is
// awaited, and started on the borrowed worker by co_await. An empty stop token
// makes it a plain timer wait; a stoppable one also cancels the timer when stop
// is requested.
class sleep_operation final {
public:
    sleep_operation(const sleep_operation&) = delete;
    sleep_operation& operator=(const sleep_operation&) = delete;
    sleep_operation(sleep_operation&& other) noexcept;
    sleep_operation& operator=(sleep_operation&&) = delete;
    void operator&() const = delete;

    [[nodiscard]] bool await_ready();
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
    [[nodiscard]] timer_sleep_result await_resume() const noexcept;

private:
    friend sleep_operation sleep_for(const worker_ref&, timeout_duration, stop_token);
    friend sleep_operation sleep_until(
        const worker_ref&, std::chrono::steady_clock::time_point, stop_token);

    sleep_operation(const worker_ref& worker_value, timeout_duration duration,
        stop_token stop_token_value) noexcept
        : worker_(&worker_value),
          target_(duration),
          stop_token_(std::move(stop_token_value)) {}

    sleep_operation(const worker_ref& worker_value, std::chrono::steady_clock::time_point deadline,
        stop_token stop_token_value) noexcept
        : worker_(&worker_value),
          target_(deadline),
          stop_token_(std::move(stop_token_value)) {}

    const worker_ref* worker_;
    std::variant<timeout_duration, std::chrono::steady_clock::time_point> target_;
    stop_token stop_token_;
    std::coroutine_handle<> continuation_{};
    worker_timer_outcome outcome_{worker_timer_outcome::expired};
    bool started_{false};
    worker_timer_registration registration_;
    // Declared last so callback teardown completes before the timer registration
    // and the borrowed worker begin destruction.
    stop_registration stop_registration_;
};

}  // namespace ruvia
