#pragma once

#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/core/detail/suspend_race_state.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait_result.h"

// Building blocks of a worker-bound wait with a single pending waiter, as used
// by channel and one_shot. A composed primitive owns a shared domain state
// with a worker_wait_slot, and each of its cold wait operations owns a
// worker_single_wait_awaiter by value:
//
//   await_ready    start(), then under lock_state() either complete_ready()
//                  with an outcome the domain already has, or
//                  complete_or_publish() to become the pending waiter;
//   await_suspend  suspend();
//   await_resume   take_result().
//
// Domain events complete the pending waiter through the slot under the same
// lock; a worker_shutdown_registration routes worker_stopping there.

namespace ruvia {

template <typename t_type, typename state_type>
    requires std::is_trivially_move_constructible_v<t_type>
class worker_single_wait_awaiter;
template <typename t_type, typename state_type>
    requires std::is_trivially_move_constructible_v<t_type>
class worker_wait_slot;

// A shared owner that links itself into its worker's shutdown registry
// through a worker_shutdown_registration member named
// shutdown_registration_ (see make_worker_wait_state()).
template <typename state_type>
concept worker_shutdown_registered =
    worker_shutdown_observer<state_type> &&
    std::same_as<decltype(state_type::shutdown_registration_), worker_shutdown_registration>;

// The domain state contract: the bound worker, the lock guarding the slot,
// the slot itself, and the shutdown registration that routes the worker's
// stopping notification to the slot. Every slot member is called with that
// lock held.
template <typename state_type, typename t_type>
concept worker_wait_state =
    std::is_trivially_move_constructible_v<t_type> && worker_shutdown_registered<state_type> &&
    requires(state_type& state, const state_type& const_state) {
        { const_state.worker() } -> std::same_as<const worker_ref&>;
        state.lock_state();
        { state.wait_slot() } -> std::same_as<worker_wait_slot<t_type, state_type>&>;
    };

// The single pending-waiter attachment of a domain state. The state owns it by
// value next to its domain fields and guards it with its own lock; every
// member is called with that lock held. Every terminal source completes the
// waiter through this one transition, so detachment, generation invalidation,
// result publication, and wake ordering cannot drift between owners.
template <typename t_type, typename state_type>
    requires std::is_trivially_move_constructible_v<t_type>
class worker_wait_slot final {
public:
    using awaiter_type = worker_single_wait_awaiter<t_type, state_type>;

    worker_wait_slot() noexcept = default;
    worker_wait_slot(const worker_wait_slot&) = delete;
    worker_wait_slot& operator=(const worker_wait_slot&) = delete;

    [[nodiscard]] bool occupied() const noexcept {
        return waiter_ != nullptr;
    }

    // Transfers result_value to the attached waiter, which must exist. A
    // throwing payload move leaves the waiter attached and pending. The wake
    // runs before the caller releases its lock: afterwards an already
    // in-flight timer expiry can resume the receiver and destroy the awaiter,
    // so reading its timer registration later would be a use-after-free.
    void complete(worker_wait_result<t_type>&& result_value) {
        auto* waiter = waiter_;
        if (waiter == nullptr) {
            throw std::logic_error("worker wait slot has no pending waiter");
        }
        const bool wake = waiter->complete_result(std::move(result_value));
        detach_all();
        if (wake) {
            waiter->wake();
        }
    }

    // Completes the attached waiter, if any, with a terminal status.
    void complete(worker_wait_status status) noexcept {
        auto* waiter = std::exchange(waiter_, nullptr);
        generation_ = 0;
        if (waiter != nullptr && waiter->complete_status(status)) {
            waiter->wake();
        }
    }

private:
    friend awaiter_type;

    [[nodiscard]] std::uint64_t reserve_generation() noexcept {
        if (++next_generation_ == 0) {
            ++next_generation_;
        }
        return next_generation_;
    }

    void publish(awaiter_type& waiter, std::uint64_t generation) noexcept {
        waiter_ = &waiter;
        generation_ = generation;
    }

    [[nodiscard]] bool holds(const awaiter_type* waiter) const noexcept {
        return waiter_ == waiter;
    }

    // Detaches waiter without completing it; returns false when it was no
    // longer attached.
    [[nodiscard]] bool detach(const awaiter_type* waiter) noexcept {
        if (waiter_ != waiter) {
            return false;
        }
        detach_all();
        return true;
    }

    // A stale generation belongs to a wait that already completed.
    void cancel(std::uint64_t generation) noexcept {
        if (waiter_ != nullptr && generation_ == generation) {
            complete(worker_wait_status::cancelled);
        }
    }

    void detach_all() noexcept {
        waiter_ = nullptr;
        generation_ = 0;
    }

    awaiter_type* waiter_{nullptr};
    std::uint64_t generation_{0};
    std::uint64_t next_generation_{0};
};

// Shared storage and transition machinery for one worker-bound waiter. Domain
// awaiters own this component by value and publish its typed address directly,
// without an outer-owner pointer, allocation, virtual dispatch, or type erasure.
// state_type must satisfy worker_wait_state<state_type, t_type>.
//
// A relative timeout starts when the wait starts; an absolute deadline that has
// already passed completes the wait as timed_out without suspending. Once the worker's timer
// queue stops, a timed wait completes as worker_stopping.
//
// The awaiter is cold until start(): nothing publishes its address before
// then, so a cold awaiter may move; a started one must stay address-stable.
template <typename t_type, typename state_type>
    requires std::is_trivially_move_constructible_v<t_type>
class worker_single_wait_awaiter final {
public:
    worker_single_wait_awaiter(std::shared_ptr<state_type> state_value, operation_options options) noexcept
        : state_(std::move(state_value)),
          timeout_(options.timeout_),
          stop_token_(std::move(options.stop_token_)) {
        static_assert(worker_wait_state<state_type, t_type>);
    }

    worker_single_wait_awaiter(const worker_single_wait_awaiter&) = delete;
    worker_single_wait_awaiter& operator=(const worker_single_wait_awaiter&) = delete;
    worker_single_wait_awaiter(worker_single_wait_awaiter&& other) noexcept
        : state_(std::move(other.state_)),
          timeout_(other.timeout_),
          deadline_(other.deadline_),
          stop_token_(std::move(other.stop_token_)) {
        if (other.started_) {
            std::terminate();
        }
    }
    worker_single_wait_awaiter& operator=(worker_single_wait_awaiter&&) = delete;

    // The first step of every domain await_ready: a wait starts once, on the
    // worker its state is bound to. A moved-from awaiter has no state.
    void start(const char* affinity_message) {
        if (started_ || !state_) {
            throw std::logic_error("worker wait cannot be awaited");
        }
        if (!state_->worker().is_current()) {
            throw std::logic_error(affinity_message);
        }
        started_ = true;
        if (timeout_.has_value()) {
            deadline_ = timeout_->deadline_from(std::chrono::steady_clock::now());
        }
    }

    [[nodiscard]] state_type& state() const noexcept {
        return *state_;
    }

    // Completes a started wait from await_ready, before it can suspend, with
    // an outcome the domain already holds.
    void complete_ready(worker_wait_result<t_type>&& result_value) {
        (void)complete_result(std::move(result_value));
    }

    void complete_ready(worker_wait_status status) {
        (void)complete_result(worker_wait_result<t_type>::with_status(status));
    }

    // The pending-receiver step shared by every domain awaiter, called with the
    // state's lock held once the domain found nothing to deliver: completes an
    // already stopped or expired wait, otherwise publishes it as the single
    // pending receiver. Returns true when the wait completed without pending.
    [[nodiscard]] bool complete_or_publish(const char* occupied_message) {
        if (stop_token_.stop_requested()) {
            complete_ready(worker_wait_status::cancelled);
            return true;
        }
        if (deadline_.has_value() && *deadline_ <= std::chrono::steady_clock::now()) {
            complete_ready(worker_wait_status::timed_out);
            return true;
        }
        if (state().wait_slot().occupied()) {
            throw std::logic_error(occupied_message);
        }
        publish();
        return false;
    }

    [[nodiscard]] bool suspend(std::coroutine_handle<> continuation) {
        auto& owner_value = state();
        auto* waiter = this;
        bool suspended = false;
        {
            auto lock = owner_value.lock_state();
            auto& slot = owner_value.wait_slot();
            // Arm the deadline before publishing the continuation: the timer only
            // fires on this worker after suspension, and a stopped timer queue can
            // still complete the wait synchronously. A published waiter that is no
            // longer attached has already completed and needs no deadline.
            if (deadline_ && slot.holds(waiter)) {
                auto status = worker_timer_schedule_status::worker_stopping;
                try {
                    status = owner_value.worker().schedule_timer(timer_, *deadline_,
                        [waiter](worker_timer_outcome outcome) noexcept {
                            waiter->on_timer_completed(outcome);
                        });
                } catch (...) {
                    (void)slot.detach(waiter);
                    throw;
                }
                if (status == worker_timer_schedule_status::worker_stopping) {
                    (void)slot.detach(waiter);
                    (void)complete_status(worker_wait_status::worker_stopping);
                }
            }
            // Completion may arrive from another thread. Publish the
            // continuation under the same lock as the result transition.
            suspended = completion_.suspend(continuation);
        }
        // Registered only once the wait really suspends. A stop requested since
        // await_ready runs the callback inline, which completes the wait as
        // cancelled under the state lock before request_stop() returns, so a
        // value sent on the same stack afterwards is queued, not delivered to a
        // cancelled receiver. The callback fits the inline storage of
        // move_only_function, so registration cannot fail.
        if (suspended && generation_ != 0) {
            register_cancellation();
        }
        return suspended;
    }

    [[nodiscard]] worker_wait_result<t_type> take_result() {
        return completion_.take_value();
    }

private:
    friend class worker_wait_slot<t_type, state_type>;

    [[nodiscard]] bool complete_result(worker_wait_result<t_type>&& result_value) {
        return completion_.complete(std::move(result_value));
    }

    [[nodiscard]] bool complete_status(worker_wait_status status) noexcept {
        return completion_.complete(worker_wait_result<t_type>::with_status(status));
    }

    // The cancellation generation exists only for a pending wait.
    void publish() noexcept {
        auto& slot = state().wait_slot();
        generation_ = stop_token_.stoppable() ? slot.reserve_generation() : 0;
        slot.publish(*this, generation_);
    }

    // Completion has already detached the intrusive waiter before wake() runs.
    // A failed continuation dispatch is therefore unrecoverable and shares the
    // same explicit terminate boundary as worker_signal and timer cancellation.
    // A wake on the worker's own thread posts the resume directly.
    void wake() noexcept {
        if (timer_.registered()) {
            timer_.cancel();
        }
        state().worker().defer_resume(completion_.continuation());
    }

    void on_timer_completed(worker_timer_outcome outcome) noexcept {
        std::coroutine_handle<> continuation;
        {
            auto& owner_value = state();
            auto state_lock = owner_value.lock_state();
            // Still pending: a cancellation reaching a pending wait comes only
            // from stop_timers().
            if (!owner_value.wait_slot().detach(this)) {
                return;
            }
            if (!complete_status(outcome == worker_timer_outcome::expired ? worker_wait_status::timed_out
                                                                          : worker_wait_status::worker_stopping)) {
                return;
            }
            continuation = completion_.continuation();
        }
        continuation.resume();
    }

    static void request_cancellation(state_type& state_value, std::uint64_t generation) {
        auto lock = state_value.lock_state();
        state_value.wait_slot().cancel(generation);
    }

    // The callback borrows the awaiter's state_, which outlives the
    // registration (its teardown waits for a running callback), and takes the
    // state lock itself: a stop requested from inside that lock -- a payload
    // move or destructor running under it -- would self-deadlock, exactly as
    // closing the domain from there would. The generation keeps a stale
    // callback from cancelling a later wait on the same slot.
    void register_cancellation() {
        stop_token_.register_callback(stop_registration_,
            [state_ptr = state_.get(), generation = generation_] {
                request_cancellation(*state_ptr, generation);
            });
    }

    std::shared_ptr<state_type> state_;
    std::optional<operation_timeout> timeout_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    stop_token stop_token_;
    bool started_{false};
    std::uint64_t generation_{0};
    worker_timer_registration timer_;
    detail::suspend_race_state<worker_wait_result<t_type>> completion_;
    // Last so callback unregistration completes before the state, timer, and
    // borrowed dispatcher begin destruction.
    stop_registration stop_registration_;
};

// Allocates a worker-bound domain state and links its shutdown_registration_
// into the worker's shutdown registry, so the worker's stopping notification
// reaches every state whose endpoints are still alive. The state's constructor
// receives args and must bind the worker that worker() returns. A stopping
// worker throws std::runtime_error.
template <typename state_type, typename... arg_types>
    requires worker_shutdown_registered<state_type> && requires(const state_type& state) {
        { state.worker() } -> std::same_as<const worker_ref&>;
    }
[[nodiscard]] std::shared_ptr<state_type> make_worker_wait_state(arg_types&&... args) {
    auto state_value = std::make_shared<state_type>(std::forward<arg_types>(args)...);
    if (state_value->worker().register_shutdown(state_value, &state_type::shutdown_registration_) !=
        worker_registration_status::registered) {
        throw std::runtime_error("cannot register state on a stopping worker");
    }
    return state_value;
}

}  // namespace ruvia
