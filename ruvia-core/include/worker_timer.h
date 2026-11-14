#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>

#include "stop_token.h"
#include "timeout_duration.h"

namespace ruvia::detail {
class worker_dispatcher;
struct timer_slot;
}  // namespace ruvia::detail

namespace ruvia {

class worker_ref;

class worker_timer_cancellation final {
public:
    worker_timer_cancellation() noexcept = default;

    void cancel() const noexcept;
    [[nodiscard]] bool valid() const noexcept {
        return dispatcher_ != nullptr && generation_ != 0;
    }

private:
    // Borrows the dispatcher kept alive by the awaiter's stable worker_ref.
    // Stop callback teardown synchronizes the borrow; cancellation dispatch owns
    // any continuation it posts.
    worker_timer_cancellation(detail::worker_dispatcher& dispatcher, std::size_t slot, std::uint64_t generation) noexcept :
        dispatcher_(&dispatcher),
        slot_(slot),
        generation_(generation) {}

    detail::worker_dispatcher* dispatcher_{nullptr};
    std::size_t slot_{0};
    std::uint64_t generation_{0};

    friend class worker_timer_registration;
};

enum class worker_timer_outcome : std::uint8_t {
    expired,
    // cancel_and_notify(), a worker_timer_cancellation, or worker shutdown
    // (stop_timers) ended the pending timer.
    cancelled,
};

// Result of worker_ref::schedule_timer(). Shutdown is a normal state, not a
// failure: once the worker's timer queue has stopped, scheduling returns
// worker_stopping, stores nothing, never invokes the completion, and leaves the
// registration unbound. That is the same terminal point a pending timer reaches
// through stop_timers(), which delivers worker_timer_outcome::cancelled; timed
// primitives map both to their own shutdown result.
// This also applies when moving the completion itself stops the timer queue.
enum class worker_timer_schedule_status : std::uint8_t {
    scheduled,
    worker_stopping,
};

// Borrowed timer token. A registration is bound to the worker of its most
// recent schedule_timer() until that timer expires, is cancelled, or is
// released by cancel()/cancel_and_notify(). Rescheduling while registration
// is in progress or the timer is still pending throws std::logic_error. Once
// it has expired or been cancelled, it may be scheduled on another worker;
// that worker observes the release with acquire/release ordering, so a caller
// that learned of the expiry through its completion may reuse it.
class worker_timer_registration final {
public:
    worker_timer_registration() noexcept = default;
    ~worker_timer_registration();

    worker_timer_registration(const worker_timer_registration&) = delete;
    worker_timer_registration& operator=(const worker_timer_registration&) = delete;
    worker_timer_registration(worker_timer_registration&&) = delete;
    worker_timer_registration& operator=(worker_timer_registration&&) = delete;

    // Removes the registration without delivering its completion: the owner
    // that cancels has stopped observing the outcome (a finished operation,
    // a re-arm, setup rollback). Destruction does the same.
    void cancel() noexcept;
    // Removes the registration and delivers worker_timer_outcome::cancelled,
    // for a completion that must run to resume or release its waiter. On the
    // worker's own thread the completion is posted rather than re-entered.
    void cancel_and_notify() noexcept;
    [[nodiscard]] worker_timer_cancellation cancellation() const&;
    worker_timer_cancellation cancellation() const&& = delete;
    // Expiry consumes the queue entry without writing through this borrowed
    // registration; its stable owner releases or reuses the token afterwards.
    [[nodiscard]] bool registered() const noexcept;

private:
    void unregister(bool notify) noexcept;
    void bind(detail::worker_dispatcher& dispatcher, std::size_t slot, std::uint64_t generation, const detail::timer_slot& slot_state) noexcept;
    void release() noexcept;
    [[nodiscard]] bool pending() const noexcept;

    // The worker_ref supplied to schedule_timer() must outlive this registration.
    // Destruction only removes it; it never queues a callback referencing the
    // destroyed owner.
    detail::worker_dispatcher* dispatcher_{nullptr};
    // Owned by that dispatcher; its activity is readable from any thread.
    const detail::timer_slot* slot_state_{nullptr};
    std::size_t slot_{0};
    std::uint64_t generation_{0};
    bool scheduling_{false};

    friend class detail::worker_dispatcher;
};

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
[[nodiscard]] sleep_operation sleep_for(const worker_ref& worker_value, timeout_duration duration, stop_token stop_token_value = {});
sleep_operation sleep_for(worker_ref&&, timeout_duration, stop_token = {}) = delete;
sleep_operation sleep_for(const worker_ref&&, timeout_duration, stop_token = {}) = delete;
[[nodiscard]] sleep_operation sleep_until(const worker_ref& worker_value, std::chrono::steady_clock::time_point deadline, stop_token stop_token_value = {});
sleep_operation sleep_until(worker_ref&&, std::chrono::steady_clock::time_point, stop_token = {}) = delete;
sleep_operation sleep_until(const worker_ref&&, std::chrono::steady_clock::time_point, stop_token = {}) = delete;

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
    friend sleep_operation sleep_until(const worker_ref&, std::chrono::steady_clock::time_point, stop_token);

    sleep_operation(const worker_ref& worker_value, timeout_duration duration, stop_token stop_token_value) noexcept :
        worker_(&worker_value),
        target_(duration),
        stop_token_(std::move(stop_token_value)) {}

    sleep_operation(const worker_ref& worker_value, std::chrono::steady_clock::time_point deadline, stop_token stop_token_value) noexcept :
        worker_(&worker_value),
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
