#pragma once

#include <concepts>
#include <coroutine>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "byte_view.h"
#include "operation_options.h"
#include "stop_token.h"
#include "suspend_race.h"
#include "wait_result.h"
#include "worker_ref.h"
#include "worker_timer.h"

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
// lock; a detail::worker_shutdown_registration routes worker_stopping there.

namespace ruvia {

template <typename t_type, typename state_type>
    requires std::is_trivially_move_constructible_v<t_type>
class worker_single_wait_awaiter;
template <typename t_type, typename state_type>
    requires std::is_trivially_move_constructible_v<t_type>
class worker_wait_slot;

// A shared owner that links itself into its worker's shutdown registry
// through a detail::worker_shutdown_registration member named
// shutdown_registration_ (see make_worker_wait_state()).
template <typename state_type>
concept worker_shutdown_registered = detail::worker_shutdown_observer<state_type> && std::same_as<decltype(state_type::shutdown_registration_), detail::worker_shutdown_registration>;

// The domain state contract: the bound worker, the lock guarding the slot,
// the slot itself, and the shutdown registration that routes the worker's
// stopping notification to the slot. Every slot member is called with that
// lock held.
template <typename state_type, typename t_type>
concept worker_wait_state = std::is_trivially_move_constructible_v<t_type> && worker_shutdown_registered<state_type> && requires(state_type& state, const state_type& const_state) {
    { const_state.worker() } -> std::same_as<const worker_ref&>;
    state.lock_state();
    { state.wait_slot() } -> std::same_as<worker_wait_slot<t_type, state_type>&>;
};

// The single pending-waiter attachment of a domain state. The state owns it by
// value next to its domain fields and guards it with its own lock; every
// member is called with that lock held. Every terminal source completes the
// waiter through this one transition, so detachment, result publication, and
// wake ordering cannot drift between owners.
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
    void complete(wait_result<t_type>&& result_value) {
        auto* waiter = waiter_;
        if (waiter == nullptr) {
            throw std::logic_error("worker wait slot has no pending waiter");
        }
        const bool wake = waiter->complete_result(std::move(result_value));
        waiter_ = nullptr;
        if (wake) {
            waiter->wake();
        }
    }

    // Completes the attached waiter, if any, with a terminal status.
    void complete(wait_status status) noexcept {
        auto* waiter = std::exchange(waiter_, nullptr);
        if (waiter != nullptr && waiter->complete_status(status)) {
            waiter->wake();
        }
    }

private:
    friend awaiter_type;

    void publish(awaiter_type& waiter) noexcept {
        waiter_ = &waiter;
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
        waiter_ = nullptr;
        return true;
    }

    // A stop callback may outlive its wait's completion but never the awaiter
    // itself (unregistration joins a running callback), so a waiter that is no
    // longer attached has already completed.
    void cancel(const awaiter_type* waiter) noexcept {
        if (waiter_ == waiter) {
            complete(wait_status::cancelled);
        }
    }

    awaiter_type* waiter_{nullptr};
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
    worker_single_wait_awaiter(std::shared_ptr<state_type> state_value, operation_options options) noexcept :
        state_(std::move(state_value)),
        bounds_(std::move(options)) {
        static_assert(worker_wait_state<state_type, t_type>);
    }

    worker_single_wait_awaiter(const worker_single_wait_awaiter&) = delete;
    worker_single_wait_awaiter& operator=(const worker_single_wait_awaiter&) = delete;
    worker_single_wait_awaiter(worker_single_wait_awaiter&& other) noexcept :
        state_(std::move(other.state_)),
        bounds_(std::move(other.bounds_)) {}
    worker_single_wait_awaiter& operator=(worker_single_wait_awaiter&&) = delete;

    // The first step of every domain await_ready: a wait starts once, on the
    // worker its state is bound to. A moved-from awaiter has no state.
    void start(const char* affinity_message) & {
        if (!state_) {
            throw std::logic_error("worker wait cannot be awaited");
        }
        bounds_.start(state_->worker(), affinity_message);
    }
    void start(const char*) && = delete;

    // The awaiter may be the last owner of the domain state.
    [[nodiscard]] state_type& state() const& noexcept RUVIA_LIFETIMEBOUND {
        return *state_;
    }
    state_type& state() const&& = delete;

    // Completes a started wait from await_ready, before it can suspend, with
    // an outcome the domain already holds.
    void complete_ready(wait_result<t_type>&& result_value) & {
        (void)complete_result(std::move(result_value));
    }
    void complete_ready(wait_result<t_type>&&) && = delete;

    void complete_ready(wait_status status) & {
        (void)complete_result(wait_result<t_type>::with_status(status));
    }
    void complete_ready(wait_status) && = delete;

    // The pending-receiver step shared by every domain awaiter, called with the
    // state's lock held once the domain found nothing to deliver: completes an
    // already stopped or expired wait, otherwise publishes it as the single
    // pending receiver. Returns true when the wait completed without pending.
    [[nodiscard]] bool complete_or_publish(const char* occupied_message) & {
        if (const auto ended = bounds_.ended()) {
            complete_ready(*ended);
            return true;
        }
        if (state().wait_slot().occupied()) {
            throw std::logic_error(occupied_message);
        }
        state().wait_slot().publish(*this);
        return false;
    }
    bool complete_or_publish(const char*) && = delete;

    [[nodiscard]] bool suspend(std::coroutine_handle<> continuation) & {
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
            if (slot.holds(waiter)) {
                bool armed = false;
                try {
                    armed = bounds_.arm_deadline<worker_single_wait_awaiter, &worker_single_wait_awaiter::on_end>(*this);
                } catch (...) {
                    (void)slot.detach(waiter);
                    throw;
                }
                if (!armed) {
                    (void)slot.detach(waiter);
                    (void)complete_status(wait_status::worker_stopping);
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
        if (suspended && bounds_.token().stoppable()) {
            register_cancellation();
        }
        return suspended;
    }
    bool suspend(std::coroutine_handle<>) && = delete;

    [[nodiscard]] wait_result<t_type> take_result() & {
        return completion_.take_value();
    }
    wait_result<t_type> take_result() && = delete;

private:
    friend class worker_wait_slot<t_type, state_type>;

    [[nodiscard]] bool complete_result(wait_result<t_type>&& result_value) {
        return completion_.complete(std::move(result_value));
    }

    [[nodiscard]] bool complete_status(wait_status status) noexcept {
        return completion_.complete(wait_result<t_type>::with_status(status));
    }

    // Completion has already detached the intrusive waiter before wake() runs.
    // A failed continuation dispatch is therefore unrecoverable and shares the
    // same explicit terminate boundary as worker_signal and timer cancellation.
    // A wake on the worker's own thread posts the resume directly.
    void wake() noexcept {
        bounds_.cancel_deadline();
        state().worker().defer_resume(completion_.continuation());
    }

    void on_end(wait_status status) noexcept {
        auto& owner = state();
        auto lock = owner.lock_state();
        auto& slot = owner.wait_slot();
        if (slot.holds(this)) {
            slot.complete(status);
        }
    }

    // The callback borrows the awaiter's state_, which outlives the
    // registration (its teardown waits for a running callback), and takes the
    // state lock itself: a stop requested from inside that lock -- a payload
    // move or destructor running under it -- would self-deadlock, exactly as
    // closing the domain from there would.
    void register_cancellation() {
        bounds_.token().register_callback(stop_registration_, [state_ptr = state_.get(), waiter = this] {
            auto lock = state_ptr->lock_state();
            state_ptr->wait_slot().cancel(waiter);
        });
    }

    std::shared_ptr<state_type> state_;
    wait_bounds bounds_;
    detail::suspend_race_state<wait_result<t_type>> completion_;
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
    if (!detail::worker_ref_access::register_shutdown(state_value->worker(), state_value, &state_type::shutdown_registration_)) {
        throw std::runtime_error("cannot register state on a stopping worker");
    }
    return state_value;
}

}  // namespace ruvia
