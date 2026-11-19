#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "byte_view.h"
#include "move_only_function.h"
#include "operation_options.h"
#include "stop_token.h"
#include "timeout_duration.h"
#include "wait_result.h"

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

enum class worker_timer_status : std::uint8_t {
    expired,
    // cancel_and_notify(), a worker_timer_cancellation, or worker shutdown
    // (stop_timers) ended the pending timer.
    cancelled,
};

// Borrowed timer token. A registration is bound to the worker of its most
// recent schedule() until that timer expires, is cancelled, or is
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

    // Schedules this registration on `worker`, which must outlive it;
    // destruction unregisters without a late callback. Shutdown is a normal
    // state, not a failure: once that worker's timer queue has stopped (even
    // while the completion is being moved in), returns false, stores nothing,
    // never invokes the completion, and leaves the registration unbound -- the
    // same terminal point stop_timers() reaches for a pending timer by
    // delivers worker_timer_status::cancelled. Rescheduling a still-pending
    // registration throws std::logic_error; an empty worker_ref throws
    // std::invalid_argument.
    bool schedule(const worker_ref& worker, std::chrono::steady_clock::time_point deadline, move_only_function<void(worker_timer_status)> completion) &;
    bool schedule(const worker_ref&, std::chrono::steady_clock::time_point, move_only_function<void(worker_timer_status)>) && = delete;
    bool schedule(worker_ref&&, std::chrono::steady_clock::time_point, move_only_function<void(worker_timer_status)>) & = delete;
    bool schedule(const worker_ref&&, std::chrono::steady_clock::time_point, move_only_function<void(worker_timer_status)>) & = delete;

    // Removes the registration without delivering its completion: the owner
    // that cancels has stopped observing the outcome (a finished operation,
    // a re-arm, setup rollback). Destruction does the same.
    void cancel() noexcept;
    // Removes the registration and delivers worker_timer_status::cancelled,
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

    // The worker_ref supplied to schedule() must outlive this registration.
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

// The deadline half of one worker-bound wait: the operation_options it was
// started with, the deadline resolved when it starts, and the timer that ends
// it there. Stop delivery stays with the owner -- inline under its lock, or
// hopped to the worker by id -- because that is where waits differ.
//
// Declare it before the owner's stop_registration, which must be the last
// member. Completion paths that hold a lock the stop callback also takes call
// cancel_deadline(), never a stop-registration reset, which would join that
// callback and deadlock. A cold wait_bounds may move; a started one is
// referenced by its timer and must stay address-stable.
class wait_bounds final {
public:
    explicit wait_bounds(operation_options options) noexcept :
        timeout_(options.timeout_),
        token_(std::move(options.stop_token_)) {}
    wait_bounds(wait_bounds&& other) noexcept;
    wait_bounds(const wait_bounds&) = delete;
    wait_bounds& operator=(const wait_bounds&) = delete;
    wait_bounds& operator=(wait_bounds&&) = delete;

    // The first step of the owner's await_ready, on `worker`, which is borrowed
    // until destruction. Throws std::logic_error(affinity_message) off that
    // worker and std::logic_error when started twice. A relative timeout
    // starts now.
    void start(const worker_ref& worker, const char* affinity_message) &;
    void start(const worker_ref&, const char*) && = delete;
    void start(worker_ref&&, const char*) & = delete;
    void start(const worker_ref&&, const char*) & = delete;

    [[nodiscard]] const stop_token& token() const& noexcept RUVIA_LIFETIMEBOUND {
        return token_;
    }
    const stop_token& token() const&& = delete;
    [[nodiscard]] bool stop_requested() const noexcept {
        return token_.stop_requested();
    }
    [[nodiscard]] bool deadline_passed() const noexcept {
        return deadline_.expired();
    }
    // cancelled, then timed_out; empty while neither bound has ended the wait.
    [[nodiscard]] std::optional<wait_status> ended() const noexcept;

    // Arms the deadline timer: `end` runs on the worker with timed_out at the
    // deadline, or worker_stopping when the timer queue stops first. Without a
    // deadline nothing is armed, unless watch_worker: the timer then waits at
    // time_point::max() so a timer-queue stop still ends the wait. Returns
    // false, storing nothing and never running `end`, once the worker's timer
    // queue has stopped; the owner then ends the wait as worker_stopping.
    // Allocation may throw; the owner rolls back.
    template <typename owner_type, void (owner_type::*end)(wait_status) noexcept>
    [[nodiscard]] bool arm_deadline(owner_type& owner, bool watch_worker = false) & {
        return arm_deadline(std::addressof(owner), [](void* target, wait_status status) noexcept {
            (static_cast<owner_type*>(target)->*end)(status);
        }, watch_worker);
    }
    template <typename owner_type, void (owner_type::*end)(wait_status) noexcept>
    bool arm_deadline(owner_type&, bool = false) && = delete;

    void cancel_deadline() noexcept {
        timer_.cancel();
    }

    // Cancels the armed timer from any thread; `end` then runs with worker_stopping.
    [[nodiscard]] worker_timer_cancellation deadline_cancellation() const& {
        return timer_.cancellation();
    }
    worker_timer_cancellation deadline_cancellation() const&& = delete;

private:
    using end_type = void (*)(void*, wait_status) noexcept;

    [[nodiscard]] bool arm_deadline(void* owner, end_type end, bool watch_worker) &;

    std::optional<operation_timeout> timeout_;
    stop_token token_;
    operation_deadline deadline_;
    const worker_ref* worker_{nullptr};
    worker_timer_registration timer_;
};

enum class sleep_status : std::uint8_t {
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
    [[nodiscard]] sleep_status await_resume() const noexcept;

private:
    friend sleep_operation sleep_for(const worker_ref&, timeout_duration, stop_token);
    friend sleep_operation sleep_until(const worker_ref&, std::chrono::steady_clock::time_point, stop_token);

    sleep_operation(const worker_ref& worker_value, operation_timeout target, stop_token stop_token_value) noexcept :
        worker_(&worker_value),
        bounds_({.timeout_ = target, .stop_token_ = std::move(stop_token_value)}) {}

    void on_end(wait_status status) noexcept;

    const worker_ref* worker_;
    wait_bounds bounds_;
    std::coroutine_handle<> continuation_{};
    sleep_status result_{sleep_status::elapsed};
    // Declared last so callback teardown completes before the timer
    // registration and the borrowed worker begin destruction.
    stop_registration stop_registration_;
};

}  // namespace ruvia
