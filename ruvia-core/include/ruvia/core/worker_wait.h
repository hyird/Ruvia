#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/worker_ref.h"

namespace ruvia {

// Owns only a detachable owner pointer and a stable worker_ref; queued work
// retains the target so that releasing it never depends on the owner's lifetime.
template <typename owner_type>
class worker_cancellation_target;

template <typename owner_type>
[[nodiscard]] inline std::shared_ptr<worker_cancellation_target<owner_type>>
make_worker_cancellation_target(owner_type& owner_value, const worker_ref& worker_value);

template <typename owner_type>
class worker_cancellation_target final
    : public std::enable_shared_from_this<worker_cancellation_target<owner_type>> {
private:
    struct constructor_token final {};

public:
    worker_cancellation_target(
        constructor_token, owner_type& owner_value, const worker_ref& worker_value) noexcept
        : owner_(&owner_value),
          worker_(worker_value) {}
    worker_cancellation_target(const worker_cancellation_target&) = delete;
    worker_cancellation_target& operator=(const worker_cancellation_target&) = delete;
    worker_cancellation_target(worker_cancellation_target&&) = delete;
    worker_cancellation_target& operator=(worker_cancellation_target&&) = delete;
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] std::uint64_t next_operation_id() noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        if (++next_operation_id_ == 0) {
            ++next_operation_id_;
        }
        return next_operation_id_;
    }
    [[nodiscard]] std::shared_ptr<worker_cancellation_target> retain() noexcept {
        auto target = this->weak_from_this().lock();
        if (!target) {
            std::terminate();
        }
        return target;
    }
    void dispatch(std::uint64_t operation_id) noexcept {
        if (auto* owner = owner_.load(std::memory_order_acquire)) {
            owner->cancel_operation_by_id(operation_id);
        }
    }
    void detach(owner_type& owner_value) noexcept {
        auto* previous = owner_.exchange(nullptr, std::memory_order_acq_rel);
        if (previous && previous != &owner_value) {
            std::terminate();
        }
    }

private:
    friend std::shared_ptr<worker_cancellation_target<owner_type>>
    make_worker_cancellation_target<owner_type>(owner_type&, const worker_ref&);

    // Construction is restricted to make_worker_cancellation_target via constructor_token.
    std::atomic<owner_type*> owner_;
    worker_ref worker_;
    std::uint64_t next_operation_id_{0};
};

template <typename owner_type>
[[nodiscard]] inline std::shared_ptr<worker_cancellation_target<owner_type>>
make_worker_cancellation_target(owner_type& owner_value, const worker_ref& worker_value) {
    using target_type = worker_cancellation_target<owner_type>;
    return std::make_shared<target_type>(
        typename target_type::constructor_token{}, owner_value, worker_value);
}

template <typename target_type>
class worker_cancellation_dispatch final {
public:
    worker_cancellation_dispatch(std::shared_ptr<target_type> target, std::uint64_t operation_id) noexcept
        : target_(std::move(target)),
          operation_id_(operation_id) {}
    void operator()() noexcept {
        target_->dispatch(operation_id_);
    }

private:
    std::shared_ptr<target_type> target_;
    std::uint64_t operation_id_;
};

template <typename target_type>
class worker_cancellation_post final {
public:
    worker_cancellation_post(target_type* target, std::uint64_t operation_id) noexcept
        : target_(target),
          operation_id_(operation_id) {
        if (!target_) {
            std::terminate();
        }
    }
    void operator()() noexcept {
        if (target_->worker().is_current()) {
            target_->dispatch(operation_id_);
            return;
        }
        auto retained = target_->retain();
        // Cancellation belongs to an already admitted operation. Use the
        // dispatcher completion lane so a full user queue cannot reject it.
        // Detached endpoints have already retired their owners and operations.
        (void)retained->worker().defer_completion(worker_cancellation_dispatch<target_type>(retained, operation_id_));
    }

private:
    target_type* target_;
    std::uint64_t operation_id_;
};

template <typename target_type>
inline constexpr bool worker_cancellation_post_is_inline =
    detail::move_only_function_fits_inline<worker_cancellation_post<target_type>> &&
    detail::move_only_function_fits_inline<worker_cancellation_dispatch<target_type>>;

// Worker-affine registration owns the supplied identity until reset(). Bind
// owner state before arm(), which may deliver an already-requested stop inline.
// reset() joins the callback before clearing its identity; a late posted
// cancellation cannot address a reused slot.
// The target is borrowed without retaining it; its owner must keep it alive
// until reset() has joined the callback.
template <typename target_type>
class worker_cancellation_registration final {
    static_assert(worker_cancellation_post_is_inline<target_type>);

public:
    worker_cancellation_registration(const std::shared_ptr<target_type>& target, std::uint64_t& identity)
        : target_(target.get()),
          identity_(&identity),
          id_(require_target(target).next_operation_id()) {
        identity = id_;
    }
    worker_cancellation_registration(std::shared_ptr<target_type>&&, std::uint64_t&) = delete;
    worker_cancellation_registration(const std::shared_ptr<target_type>&&, std::uint64_t&) = delete;
    worker_cancellation_registration(const worker_cancellation_registration&) = delete;
    worker_cancellation_registration& operator=(const worker_cancellation_registration&) = delete;
    worker_cancellation_registration(worker_cancellation_registration&&) = delete;
    worker_cancellation_registration& operator=(worker_cancellation_registration&&) = delete;
    ~worker_cancellation_registration() {
        reset();
    }
    [[nodiscard]] std::uint64_t id() const noexcept {
        return id_;
    }
    void arm(const stop_token& token) {
        if (!target_ || armed_) {
            std::terminate();
        }
        armed_ = true;
        if (token.stoppable()) {
            // stop_token delivers an already-requested stop inline and invokes
            // a linked callback at most once. A second stop_requested dispatch
            // would duplicate cancellation and race a foreign queued delivery.
            token.register_callback(registration_, worker_cancellation_post<target_type>(target_, id_));
        }
    }
    void reset() noexcept {
        registration_.reset();
        if (identity_ && *identity_ == id_) {
            *identity_ = 0;
        }
        identity_ = nullptr;
        target_ = nullptr;
    }

private:
    static target_type& require_target(const std::shared_ptr<target_type>& target) {
        if (!target || !target->worker().is_current()) {
            std::terminate();
        }
        return *target;
    }
    target_type* target_;
    std::uint64_t* identity_{};
    std::uint64_t id_;
    bool armed_{};
    stop_registration registration_;
};

}  // namespace ruvia

namespace ruvia::detail {

struct suspend_race_pending final {};

// A completion can race the suspension of the awaiter's coroutine in two
// orders: complete-then-suspend and suspend-then-complete. A caller that
// initiates an asynchronous operation must not resume the continuation from
// inside await_suspend before that coroutine is actually suspended; driving
// both orderings through this state machine makes the await_suspend return
// value (suspend or not) the single verdict on which ordering happened.
//
// Payload moves may throw; complete() then restores the state it found so the
// awaiter can retry or propagate the failure from the original call site.
// Calls must be serialized by the owner, either on one thread or under its lock.
template <typename t_type>
class suspend_race_state final {
public:
    suspend_race_state() = default;
    suspend_race_state(const suspend_race_state&) = delete;
    suspend_race_state& operator=(const suspend_race_state&) = delete;
    suspend_race_state(suspend_race_state&&) = delete;
    suspend_race_state& operator=(suspend_race_state&&) = delete;

    // Called from await_suspend after the operation was initiated: publishes
    // the continuation when still pending, or reports that the operation
    // already completed synchronously (its result is consumed by take_value()).
    [[nodiscard]] bool suspend(std::coroutine_handle<> continuation) noexcept {
        if (std::holds_alternative<suspend_race_pending>(state_)) {
            state_.template emplace<suspend_race_suspended_type>(continuation);
            return true;
        }
        if (std::holds_alternative<suspend_race_ready_before_suspend_type>(state_)) {
            return false;
        }
        std::terminate();
    }

    // Returns true only when the suspended coroutine now needs an explicit
    // wake. A completion racing before await_suspend is consumed there instead.
    [[nodiscard]] bool complete(t_type&& value) {
        if (std::holds_alternative<suspend_race_pending>(state_)) {
            try {
                state_.template emplace<suspend_race_ready_before_suspend_type>(std::move(value));
            } catch (...) {
                state_.template emplace<suspend_race_pending>();
                throw;
            }
            return false;
        }
        if (auto* suspended = std::get_if<suspend_race_suspended_type>(&state_)) {
            const auto continuation = suspended->continuation();
            try {
                state_.template emplace<suspend_race_ready_after_suspend_type>(
                    std::move(value), continuation);
            } catch (...) {
                state_.template emplace<suspend_race_suspended_type>(continuation);
                throw;
            }
            return true;
        }
        std::terminate();
    }

    [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
        const auto* ready = std::get_if<suspend_race_ready_after_suspend_type>(&state_);
        if (ready == nullptr) {
            std::terminate();
        }
        return ready->continuation();
    }

    [[nodiscard]] t_type take_value() noexcept(std::is_nothrow_move_constructible_v<t_type>) {
        if (auto* ready = std::get_if<suspend_race_ready_before_suspend_type>(&state_)) {
            return std::move(*ready).take_value();
        }
        if (auto* ready = std::get_if<suspend_race_ready_after_suspend_type>(&state_)) {
            return std::move(*ready).take_value();
        }
        std::terminate();
    }

private:
    class suspend_race_suspended_type final {
    public:
        explicit suspend_race_suspended_type(std::coroutine_handle<> continuation) noexcept
            : continuation_(continuation) {}

        [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
            return continuation_;
        }

    private:
        std::coroutine_handle<> continuation_;
    };

    class suspend_race_ready_before_suspend_type final {
    public:
        explicit suspend_race_ready_before_suspend_type(t_type&& value) noexcept(
            std::is_nothrow_move_constructible_v<t_type>)
            : value_(std::move(value)) {}

        [[nodiscard]] t_type take_value() && noexcept(std::is_nothrow_move_constructible_v<t_type>) {
            return std::move(value_);
        }

    private:
        t_type value_;
    };

    class suspend_race_ready_after_suspend_type final {
    public:
        suspend_race_ready_after_suspend_type(t_type&& value, std::coroutine_handle<> continuation) noexcept(
            std::is_nothrow_move_constructible_v<t_type>)
            : value_(std::move(value)),
              continuation_(continuation) {}

        [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
            return continuation_;
        }

        [[nodiscard]] t_type take_value() && noexcept(std::is_nothrow_move_constructible_v<t_type>) {
            return std::move(value_);
        }

    private:
        t_type value_;
        std::coroutine_handle<> continuation_;
    };

    using state_type = std::variant<suspend_race_pending, suspend_race_suspended_type,
        suspend_race_ready_before_suspend_type, suspend_race_ready_after_suspend_type>;

    state_type state_;
};

}  // namespace ruvia::detail

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

namespace ruvia {

enum class one_shot_complete_status : std::uint8_t {
    completed,
    already_completed,
    in_progress,
    receiver_closed,
    worker_stopping,
};

template <typename t_type>
class one_shot_completion;

template <typename t_type>
using one_shot_complete_result =
    worker_delivery_result<t_type, one_shot_complete_status, one_shot_complete_status::completed>;

namespace detail {

struct one_shot_pending final {};
struct one_shot_completing final {};
struct one_shot_consumed final {};
struct one_shot_receiver_closed final {};
struct one_shot_worker_stopping final {};

struct one_shot_ready final {};

template <typename t_type>
struct one_shot_state final {
    explicit one_shot_state(worker_ref target)
        : worker_(std::move(target)) {
        if (!worker_.valid()) {
            throw std::invalid_argument("one-shot requires a valid worker");
        }
    }

    void worker_stopping() noexcept;

    [[nodiscard]] std::unique_lock<std::mutex> lock_state() {
        return std::unique_lock(mutex_);
    }
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] worker_wait_slot<std::monostate, one_shot_state>& wait_slot() noexcept {
        return wait_slot_;
    }
    [[nodiscard]] t_type take_ready_value() {
        struct ready_consumption final {
            std::optional<t_type>& value_;
            ~ready_consumption() {
                value_.reset();
            }
        } consumption{ready_value_};
        return std::move(*ready_value_);
    }

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_registration shutdown_registration_;
    worker_ref worker_;
    std::mutex mutex_;
    using lifecycle_type = std::variant<one_shot_pending, one_shot_completing, one_shot_ready,
        one_shot_consumed, one_shot_receiver_closed, one_shot_worker_stopping>;
    lifecycle_type lifecycle_;
    // After publishing consumed/closed, the thread that claimed ready owns this
    // stable slot until its value has been moved or destroyed outside mutex_.
    std::optional<t_type> ready_value_;
    worker_wait_slot<std::monostate, one_shot_state<t_type>> wait_slot_;
};

template <typename t_type>
void one_shot_state<t_type>::worker_stopping() noexcept {
    std::lock_guard lock(mutex_);
    // Delivered, consumed and closed outcomes stay terminal; only an
    // unresolved exchange is ended by worker shutdown.
    if (std::holds_alternative<one_shot_pending>(lifecycle_) ||
        std::holds_alternative<one_shot_completing>(lifecycle_)) {
        lifecycle_.template emplace<one_shot_worker_stopping>();
    }
    wait_slot_.complete(worker_wait_status::worker_stopping);
}

}  // namespace detail

template <typename t_type>
class one_shot_completion final {
public:
    one_shot_completion() noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

    // Keep this completion handle alive through the call, including payload
    // moves and destruction. Concurrent attempts report in_progress while the
    // first value is being constructed.
    [[nodiscard]] one_shot_complete_result<t_type> complete(t_type value) const {
        // Payload code may synchronously rebind this handle while the call is active.
        const auto state = state_;
        if (state == nullptr) {
            return detail::worker_delivery_access::reject<one_shot_complete_result<t_type>>(
                one_shot_complete_status::receiver_closed, std::move(value));
        }
        std::optional<one_shot_complete_status> rejection;
        {
            std::lock_guard lock(state->mutex_);
            if (std::holds_alternative<detail::one_shot_ready>(state->lifecycle_) ||
                std::holds_alternative<detail::one_shot_consumed>(state->lifecycle_)) {
                rejection = one_shot_complete_status::already_completed;
            } else if (std::holds_alternative<detail::one_shot_completing>(state->lifecycle_)) {
                rejection = one_shot_complete_status::in_progress;
            } else if (std::holds_alternative<detail::one_shot_worker_stopping>(state->lifecycle_)) {
                rejection = one_shot_complete_status::worker_stopping;
            } else if (std::holds_alternative<detail::one_shot_receiver_closed>(state->lifecycle_)) {
                rejection = one_shot_complete_status::receiver_closed;
            } else if (!state->worker_.accepting()) {
                rejection = one_shot_complete_status::worker_stopping;
            } else {
                assert(std::holds_alternative<detail::one_shot_pending>(state->lifecycle_));
                state->lifecycle_.template emplace<detail::one_shot_completing>();
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<one_shot_complete_result<t_type>>(*rejection, std::move(value));
        }
        try {
            state->ready_value_.emplace(std::move(value));
        } catch (...) {
            std::lock_guard lock(state->mutex_);
            if (std::holds_alternative<detail::one_shot_completing>(state->lifecycle_)) {
                state->lifecycle_.template emplace<detail::one_shot_pending>();
            }
            throw;
        }
        {
            std::lock_guard lock(state->mutex_);
            if (std::holds_alternative<detail::one_shot_receiver_closed>(state->lifecycle_)) {
                rejection = one_shot_complete_status::receiver_closed;
            } else if (std::holds_alternative<detail::one_shot_worker_stopping>(state->lifecycle_) ||
                       !state->worker_.accepting()) {
                state->lifecycle_.template emplace<detail::one_shot_worker_stopping>();
                state->wait_slot_.complete(worker_wait_status::worker_stopping);
                rejection = one_shot_complete_status::worker_stopping;
            } else if (state->wait_slot_.occupied()) {
                state->lifecycle_.template emplace<detail::one_shot_consumed>();
                state->wait_slot_.complete(worker_wait_result<std::monostate>::with_value(std::monostate{}));
            } else {
                state->lifecycle_.template emplace<detail::one_shot_ready>();
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<one_shot_complete_result<t_type>>(*rejection, state->take_ready_value());
        }
        return detail::worker_delivery_access::accept<one_shot_complete_result<t_type>>();
    }

private:
    explicit one_shot_completion(std::shared_ptr<detail::one_shot_state<t_type>> state_value)
        : state_(std::move(state_value)) {}
    std::shared_ptr<detail::one_shot_state<t_type>> state_;
    template <typename u_type>
    friend auto make_one_shot(worker_ref);
};

template <typename t_type>
class one_shot_receiver final {
public:
    one_shot_receiver() = delete;
    one_shot_receiver(const one_shot_receiver&) = delete;
    one_shot_receiver& operator=(const one_shot_receiver&) = delete;
    one_shot_receiver(one_shot_receiver&&) noexcept = default;
    one_shot_receiver& operator=(one_shot_receiver&&) = delete;
    ~one_shot_receiver() {
        close();
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }
    // The wait operation: single-use, movable only before it is awaited, and
    // started on the receiver's worker by co_await. Dropping a cold one has no
    // effect on the exchange.
    class wait_operation final {
    public:
        wait_operation(wait_operation&&) noexcept = default;
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        wait_operation& operator=(wait_operation&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready() {
            wait_.start("one-shot wait must run on its bound worker");
            auto& owner_value = wait_.state();
            auto lock = owner_value.lock_state();
            if (std::holds_alternative<detail::one_shot_ready>(owner_value.lifecycle_)) {
                owner_value.lifecycle_.template emplace<detail::one_shot_consumed>();
                wait_.complete_ready(worker_wait_result<std::monostate>::with_value(std::monostate{}));
                return true;
            }
            if (std::holds_alternative<detail::one_shot_worker_stopping>(owner_value.lifecycle_)) {
                wait_.complete_ready(worker_wait_status::worker_stopping);
                return true;
            }
            if (std::holds_alternative<detail::one_shot_receiver_closed>(owner_value.lifecycle_) ||
                std::holds_alternative<detail::one_shot_consumed>(owner_value.lifecycle_)) {
                wait_.complete_ready(worker_wait_status::closed);
                return true;
            }
            assert(std::holds_alternative<detail::one_shot_pending>(owner_value.lifecycle_) ||
                   std::holds_alternative<detail::one_shot_completing>(owner_value.lifecycle_));
            return wait_.complete_or_publish("one-shot supports one pending receiver");
        }

        bool await_suspend(std::coroutine_handle<> continuation) {
            return wait_.suspend(continuation);
        }

        [[nodiscard]] worker_wait_result<t_type> await_resume() {
            auto result = wait_.take_result();
            if (!result.has_value()) {
                return worker_wait_result<t_type>::with_status(result.status());
            }
            return worker_wait_result<t_type>::with_value(wait_.state().take_ready_value());
        }

    private:
        friend class one_shot_receiver;
        using state_type = detail::one_shot_state<t_type>;

        wait_operation(std::shared_ptr<state_type> state_value, operation_options options) noexcept
            : wait_(std::move(state_value), std::move(options)) {}

        worker_single_wait_awaiter<std::monostate, state_type> wait_;
    };

    // The receiver owns the receive side: destroying it closes every cold or
    // running wait. Require a named owner so storing an operation cannot
    // silently turn a temporary receiver into an already-closed operation.
    // A relative options.timeout_ bounds the wait from its start; a deadline
    // that has passed reports timed_out unless a value is ready.
    [[nodiscard]] wait_operation wait(operation_options options = {}) const& {
        return wait_operation(state_, std::move(options));
    }
    wait_operation wait(operation_options = {}) const&& = delete;

    // The worker every wait must run on. Makes the receive-side affinity
    // contract queryable instead of only failing at await time. A moved-from
    // receiver has no bound worker and cannot answer this query.
    [[nodiscard]] const worker_ref& worker() const& noexcept {
        if (!state_) {
            std::terminate();
        }
        return state_->worker_;
    }
    const worker_ref& worker() const&& = delete;

    // Closing discards an unconsumed value before returning and closes every
    // future wait. Its destruction may reenter this receiver or its completion.
    void close() const {
        if (!state_) {
            return;
        }
        const auto state = state_;
        bool discard = false;
        {
            std::lock_guard lock(state->mutex_);
            discard = std::holds_alternative<detail::one_shot_ready>(state->lifecycle_);
            if (!discard && !std::holds_alternative<detail::one_shot_pending>(state->lifecycle_) &&
                !std::holds_alternative<detail::one_shot_completing>(state->lifecycle_)) {
                return;
            }
            state->lifecycle_.template emplace<detail::one_shot_receiver_closed>();
            state->wait_slot_.complete(worker_wait_status::closed);
        }
        if (discard) {
            state->ready_value_.reset();
        }
    }

private:
    explicit one_shot_receiver(std::shared_ptr<detail::one_shot_state<t_type>> state_value)
        : state_(std::move(state_value)) {}
    std::shared_ptr<detail::one_shot_state<t_type>> state_;
    template <typename u_type>
    friend auto make_one_shot(worker_ref);
};

template <typename t_type>
// The value parameter accepts both lvalue and rvalue worker refs before
// transferring the stable dispatcher endpoint into the one-shot state.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
[[nodiscard]] auto make_one_shot(worker_ref worker_value) {
    auto state_value = make_worker_wait_state<detail::one_shot_state<t_type>>(std::move(worker_value));
    one_shot_completion<t_type> completion(state_value);
    return std::pair(std::move(completion), one_shot_receiver<t_type>(std::move(state_value)));
}

}  // namespace ruvia

namespace ruvia {
class pool_lease_acquire;

// Single-worker owner of pool slot leases and their asynchronous wait queue.
// The worker endpoint is retained once by the scheduler. Acquires copy
// their inputs before return and borrow only their scheduler, which must
// outlive them.
class pool_lease_scheduler final {
public:
    pool_lease_scheduler(std::size_t pool_size, const worker_ref& worker_value);

    pool_lease_scheduler(const pool_lease_scheduler&) = delete;
    pool_lease_scheduler& operator=(const pool_lease_scheduler&) = delete;
    pool_lease_scheduler(pool_lease_scheduler&&) = delete;
    pool_lease_scheduler& operator=(pool_lease_scheduler&&) = delete;
    ~pool_lease_scheduler();

    // The returned awaitable is cold: nothing is checked or queued until it is
    // awaited, which must happen on the scheduler's worker. A free slot is
    // leased synchronously without suspending; only a saturated pool waits.
    // The result carries the leased slot index as its value, or closed,
    // worker_stopping, timed_out or cancelled. A timed-out waiter leaves other
    // queued acquisitions unchanged. A relative options.timeout_ bounds the
    // wait from await; a deadline that has passed times out a saturated
    // acquire at once. Once the worker's shutdown notification arrives or its
    // timer queue stops, queued acquires and saturated new ones complete with
    // worker_stopping, as channel and one_shot waits do; a free slot is still
    // leased. It never throws for that.
    [[nodiscard]] pool_lease_acquire acquire(operation_options options = {}) &;
    pool_lease_acquire acquire(operation_options = {}) && = delete;

    // Return a caller-owned slot on the bound worker. A queued acquire takes
    // ownership before this call returns, but its coroutine resumes in a later
    // worker turn, never inline here. Later cancellation or close() cannot
    // replace that committed acquisition.
    void release(std::size_t slot) noexcept;
    // Refuses new acquires and completes every queued acquire with closed;
    // returns false when already closing. Like release(), the queued coroutines
    // resume in a later worker turn, never inline here, so the caller finishes
    // its own teardown before any waiter observes the closure.
    [[nodiscard]] bool close() noexcept;
    [[nodiscard]] bool closing() const noexcept;

private:
    friend class pool_lease_acquire;

    class impl_type;
    std::unique_ptr<impl_type> impl_;
};

// One acquire: single-use, movable only before it is awaited. The scheduler
// must outlive it; destroying a scheduler with live acquires terminates.
class pool_lease_acquire final {
public:
    pool_lease_acquire(pool_lease_acquire&& other) noexcept;
    pool_lease_acquire(const pool_lease_acquire&) = delete;
    pool_lease_acquire& operator=(const pool_lease_acquire&) = delete;
    pool_lease_acquire& operator=(pool_lease_acquire&&) = delete;
    void operator&() const = delete;
    ~pool_lease_acquire();

    [[nodiscard]] bool await_ready();
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
    [[nodiscard]] worker_wait_result<std::size_t> await_resume();

private:
    friend class pool_lease_scheduler;

    pool_lease_acquire(pool_lease_scheduler::impl_type& owner_value, std::optional<operation_timeout> timeout,
        stop_token stop_token_value) noexcept;
    void cancel_registrations() noexcept;
    void complete(worker_wait_result<std::size_t> result_value) noexcept;
    [[nodiscard]] std::coroutine_handle<> resolve(worker_wait_result<std::size_t> result_value) noexcept;
    [[nodiscard]] std::coroutine_handle<> resolve(worker_wait_status status) noexcept;

    pool_lease_scheduler::impl_type* owner_;
    std::optional<operation_timeout> timeout_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    stop_token stop_token_;
    std::optional<worker_wait_result<std::size_t>> result_;
    std::uint64_t waiter_id_{0};
    std::coroutine_handle<> continuation_{};
    pool_lease_acquire* previous_{nullptr};
    pool_lease_acquire* next_{nullptr};
    worker_timer_registration deadline_timer_;
    stop_registration stop_registration_;
    bool queued_{false};
    bool awaited_{false};
};

}  // namespace ruvia

namespace ruvia {

enum class channel_send_status : std::uint8_t {
    sent,
    // send() only: every slot is in use.
    full,
    closed,
    worker_stopping,
    // send_wait() only: its operation_options ended the wait for a slot.
    timed_out,
    cancelled,
};

template <typename t_type>
class channel_sender;
template <typename t_type>
class channel_send_operation;

struct channel_options final {
    // Bounds buffered and reserved sends. One extra slot permits a value
    // already assigned to a receiver while the buffer fills again.
    // A send may report full while every slot is temporarily in use.
    std::size_t capacity_{0};
};

template <typename t_type>
using channel_send_result =
    worker_delivery_result<t_type, channel_send_status, channel_send_status::sent>;

namespace detail {

struct channel_open final {};
struct channel_closed final {};
struct channel_worker_stopping final {};

template <typename t_type>
struct channel_state final {
    struct node final {
        std::optional<t_type> value_;
        node* next_{nullptr};
    };

    static std::size_t slot_count(std::size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("channel capacity must be nonzero");
        }
        if (capacity == std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("channel capacity is too large");
        }
        return capacity + 1;
    }

    channel_state(worker_ref target, std::size_t requested_capacity)
        : worker_(std::move(target)),
          slots_(slot_count(requested_capacity)),
          capacity_(requested_capacity) {
        if (!worker_.valid()) {
            throw std::invalid_argument("channel requires a valid worker");
        }
        for (auto& slot : slots_) {
            slot.next_ = free_;
            free_ = &slot;
        }
    }

    [[nodiscard]] std::unique_lock<std::mutex> lock_state() {
        return std::unique_lock(mutex_);
    }

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] worker_wait_slot<node*, channel_state>& wait_slot() noexcept {
        return wait_slot_;
    }

    [[nodiscard]] std::optional<channel_send_status> check_open_locked() const noexcept {
        if (std::holds_alternative<channel_closed>(lifecycle_)) {
            return channel_send_status::closed;
        }
        if (std::holds_alternative<channel_worker_stopping>(lifecycle_) || !worker_.accepting()) {
            return channel_send_status::worker_stopping;
        }
        return std::nullopt;
    }

    // Called under mutex_. Reservations hold capacity until publication.
    [[nodiscard]] node* reserve_node() noexcept {
        if (queued_ + reserved_ == capacity_ || free_ == nullptr) {
            return nullptr;
        }
        auto* slot = free_;
        free_ = slot->next_;
        slot->next_ = nullptr;
        ++reserved_;
        return slot;
    }

    void enqueue(node* slot) noexcept {
        slot->next_ = nullptr;
        if (tail_ != nullptr) {
            tail_->next_ = slot;
        } else {
            head_ = slot;
        }
        tail_ = slot;
        ++queued_;
    }

    [[nodiscard]] node* dequeue() noexcept {
        auto* slot = head_;
        if (slot == nullptr) {
            return nullptr;
        }
        head_ = slot->next_;
        if (head_ == nullptr) {
            tail_ = nullptr;
        }
        slot->next_ = nullptr;
        --queued_;
        return slot;
    }

    void recycle(node* slot) noexcept {
        slot->next_ = free_;
        free_ = slot;
    }

    // Called under mutex_ whenever capacity may have been released: hands
    // each free slot, in FIFO order, to a waiting send_wait() as a
    // reservation it fills once it resumes on its own worker.
    void grant_send_waiters() noexcept;
    // Called under mutex_ by close and worker shutdown.
    void complete_send_waiters(channel_send_status status) noexcept;
    void enqueue_send_waiter(channel_send_operation<t_type>& waiter) noexcept;
    // Returns false when the waiter is no longer queued.
    [[nodiscard]] bool remove_send_waiter(channel_send_operation<t_type>& waiter) noexcept;

    // Called under mutex_: publishes a filled reservation to the pending
    // receiver or the queue. Returns the rejection when the channel closed or
    // stopped since the reservation, leaving the value in the slot.
    [[nodiscard]] std::optional<channel_send_status> publish_reserved(node* slot) {
        --reserved_;
        auto rejection = check_open_locked();
        if (!rejection) {
            if (wait_slot_.occupied()) {
                wait_slot_.complete(worker_wait_result<node*>::with_value(slot));
                // A direct handoff never occupied the queue.
                grant_send_waiters();
            } else {
                enqueue(slot);
            }
        }
        return rejection;
    }

    // Called under mutex_ when a filled reservation is abandoned.
    void release_reserved(node* slot) noexcept {
        --reserved_;
        recycle(slot);
        grant_send_waiters();
    }

    // Owns one reserved or claimed slot; user destruction runs without mutex_.
    class node_guard final {
    public:
        node_guard(channel_state& state, node* slot) noexcept
            : state_(state),
              slot_(slot) {}
        node_guard(const node_guard&) = delete;
        node_guard& operator=(const node_guard&) = delete;
        ~node_guard() {
            if (slot_ != nullptr) {
                slot_->value_.reset();
                auto lock = state_.lock_state();
                state_.recycle(slot_);
                state_.grant_send_waiters();
            }
        }
        void release() noexcept {
            slot_ = nullptr;
        }

    private:
        channel_state& state_;
        node* slot_;
    };

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_registration shutdown_registration_;
    worker_ref worker_;
    std::mutex mutex_;
    std::vector<node> slots_;
    const std::size_t capacity_;
    node* free_{nullptr};
    node* head_{nullptr};
    node* tail_{nullptr};
    std::size_t queued_{0};
    std::size_t reserved_{0};
    using lifecycle_type = std::variant<channel_open, channel_closed, channel_worker_stopping>;
    lifecycle_type lifecycle_;
    bool receiver_closed_{false};
    worker_wait_slot<node*, channel_state<t_type>> wait_slot_;
    channel_send_operation<t_type>* send_head_{nullptr};
    channel_send_operation<t_type>* send_tail_{nullptr};

    void close(bool retire_receiver);
    void worker_stopping() noexcept;

private:
    void discard_buffered();
};

template <typename t_type>
void channel_state<t_type>::close(bool retire_receiver) {
    {
        auto lock = lock_state();
        receiver_closed_ = receiver_closed_ || retire_receiver;
        if (std::holds_alternative<channel_open>(lifecycle_)) {
            lifecycle_.template emplace<channel_closed>();
            wait_slot_.complete(worker_wait_status::closed);
            complete_send_waiters(channel_send_status::closed);
        }
    }
    if (retire_receiver) {
        discard_buffered();
    }
}

// Retirement leaves the lifecycle closed or stopping, so no sender can publish
// again and the queue only shrinks here. Buffered values die now, on the
// retiring thread, rather than with the last sender wherever it drops. Each
// value is unpublished under the lock but destroyed in its slot after the lock
// is released, so its destructor may close this channel or drop its endpoints;
// the caller keeps the state alive until the drain returns.
template <typename t_type>
void channel_state<t_type>::discard_buffered() {
    for (;;) {
        node* retired;
        {
            auto lock = lock_state();
            retired = dequeue();
        }
        if (retired == nullptr) {
            return;
        }
        node_guard guard(*this, retired);
    }
}

template <typename t_type>
void channel_state<t_type>::worker_stopping() noexcept {
    auto lock = lock_state();
    lifecycle_.template emplace<channel_worker_stopping>();
    wait_slot_.complete(worker_wait_status::worker_stopping);
    complete_send_waiters(channel_send_status::worker_stopping);
}

}  // namespace detail

// One send_wait(): single-use, movable only before it is awaited. It owns the
// value until delivery and returns it in the result otherwise. A slot granted
// while it waits is reserved for it and filled when it resumes on the worker
// that awaited it.
template <typename t_type>
class channel_send_operation final {
public:
    channel_send_operation(const channel_send_operation&) = delete;
    channel_send_operation& operator=(const channel_send_operation&) = delete;
    channel_send_operation(channel_send_operation&& other) noexcept(std::is_nothrow_move_constructible_v<t_type>)
        : state_(std::move(other.state_)),
          value_(std::move(other.value_)),
          timeout_(other.timeout_),
          stop_token_(std::move(other.stop_token_)) {
        if (other.started_) {
            std::terminate();
        }
    }
    channel_send_operation& operator=(channel_send_operation&&) = delete;
    void operator&() const = delete;

    ~channel_send_operation() {
        // A frame destroyed while suspended must not leave a dangling waiter.
        if (queued_) {
            auto lock = state_->lock_state();
            (void)state_->remove_send_waiter(*this);
        }
    }

    [[nodiscard]] bool await_ready() {
        if (started_ || !value_.has_value()) {
            throw std::logic_error("channel send cannot be awaited");
        }
        if (!state_) {
            started_ = true;
            complete_ready({channel_send_status::closed, nullptr});
            return true;
        }
        auto worker_value = detail::worker_ref_access::current();
        if (!worker_value.valid()) {
            throw std::logic_error("channel send_wait must be awaited on a worker");
        }
        started_ = true;
        worker_ = std::move(worker_value);
        const auto now = std::chrono::steady_clock::now();
        if (timeout_.has_value()) {
            deadline_ = timeout_->deadline_from(now);
        }
        auto& state = *state_;
        auto lock = state.lock_state();
        if (const auto rejection = state.check_open_locked()) {
            complete_ready({*rejection, nullptr});
            return true;
        }
        if (auto* slot = state.reserve_node()) {
            complete_ready({channel_send_status::sent, slot});
            return true;
        }
        if (stop_token_.stop_requested()) {
            complete_ready({channel_send_status::cancelled, nullptr});
            return true;
        }
        if (deadline_.has_value() && *deadline_ <= now) {
            complete_ready({channel_send_status::timed_out, nullptr});
            return true;
        }
        state.enqueue_send_waiter(*this);
        return false;
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
        auto& state = *state_;
        bool suspended = false;
        {
            auto lock = state.lock_state();
            // A timer is always armed on the awaiting worker, at the deadline or
            // never, so that worker's timer shutdown also ends the wait.
            if (queued_) {
                auto status = worker_timer_schedule_status::worker_stopping;
                try {
                    status = worker_.schedule_timer(timer_,
                        deadline_.value_or(std::chrono::steady_clock::time_point::max()),
                        [this](worker_timer_outcome outcome) noexcept { on_timer_completed(outcome); });
                } catch (...) {
                    (void)state.remove_send_waiter(*this);
                    throw;
                }
                if (status == worker_timer_schedule_status::worker_stopping) {
                    (void)state.remove_send_waiter(*this);
                    (void)completion_.complete({channel_send_status::worker_stopping, nullptr});
                }
            }
            suspended = completion_.suspend(continuation);
        }
        // Registered only once the wait really suspends; a stop already
        // requested runs the callback inline and completes the wait.
        if (suspended && stop_token_.stoppable()) {
            stop_token_.register_callback(stop_registration_, [this] { request_cancellation(); });
        }
        return suspended;
    }

    [[nodiscard]] channel_send_result<t_type> await_resume() {
        using result_type = channel_send_result<t_type>;
        const auto outcome = completion_.take_value();
        if (outcome.status_ != channel_send_status::sent) {
            return detail::worker_delivery_access::reject<result_type>(outcome.status_, std::move(*value_));
        }
        auto& state = *state_;
        typename state_type::node_guard guard(state, outcome.slot_);
        try {
            outcome.slot_->value_.emplace(std::move(*value_));
        } catch (...) {
            auto lock = state.lock_state();
            state.release_reserved(outcome.slot_);
            guard.release();
            throw;
        }
        std::optional<channel_send_status> rejection;
        {
            auto lock = state.lock_state();
            rejection = state.publish_reserved(outcome.slot_);
            if (!rejection) {
                guard.release();
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<result_type>(*rejection, std::move(*outcome.slot_->value_));
        }
        return detail::worker_delivery_access::accept<result_type>();
    }

private:
    using state_type = detail::channel_state<t_type>;
    using node = typename state_type::node;

    friend class channel_sender<t_type>;
    friend struct detail::channel_state<t_type>;

    struct outcome_type final {
        channel_send_status status_;
        // The reserved slot when status_ is sent.
        node* slot_;
    };

    channel_send_operation(std::shared_ptr<state_type> state_value, t_type&& value, operation_options options) noexcept(
        std::is_nothrow_move_constructible_v<t_type>)
        : state_(std::move(state_value)),
          value_(std::move(value)),
          timeout_(options.timeout_),
          stop_token_(std::move(options.stop_token_)) {}

    void complete_ready(outcome_type outcome) noexcept {
        (void)completion_.complete(std::move(outcome));
    }

    // Under the state lock, once removed from the queue. The wake runs before
    // the lock is released, so a racing timer expiry cannot resume and destroy
    // this operation first.
    void complete_locked(outcome_type outcome) noexcept {
        if (completion_.complete(std::move(outcome))) {
            if (timer_.registered()) {
                timer_.cancel();
            }
            worker_.defer_resume(completion_.continuation());
        }
    }

    void on_timer_completed(worker_timer_outcome outcome) noexcept {
        std::coroutine_handle<> continuation;
        {
            auto lock = state_->lock_state();
            if (!state_->remove_send_waiter(*this)) {
                return;
            }
            if (!completion_.complete({outcome == worker_timer_outcome::expired ? channel_send_status::timed_out
                                                                                : channel_send_status::worker_stopping,
                    nullptr})) {
                return;
            }
            continuation = completion_.continuation();
        }
        continuation.resume();
    }

    // Unregistration waits for a running callback, so this operation and its
    // state outlive it.
    void request_cancellation() noexcept {
        auto lock = state_->lock_state();
        if (state_->remove_send_waiter(*this)) {
            complete_locked({channel_send_status::cancelled, nullptr});
        }
    }

    std::shared_ptr<state_type> state_;
    std::optional<t_type> value_;
    std::optional<operation_timeout> timeout_;
    stop_token stop_token_;
    worker_ref worker_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    // Queue links, guarded by the state lock.
    channel_send_operation* previous_{nullptr};
    channel_send_operation* next_{nullptr};
    bool queued_{false};
    bool started_{false};
    worker_timer_registration timer_;
    detail::suspend_race_state<outcome_type> completion_;
    // Last so callback unregistration completes before anything it touches
    // begins destruction.
    stop_registration stop_registration_;
};

namespace detail {

template <typename t_type>
void channel_state<t_type>::enqueue_send_waiter(channel_send_operation<t_type>& waiter) noexcept {
    waiter.previous_ = send_tail_;
    waiter.next_ = nullptr;
    waiter.queued_ = true;
    if (send_tail_ != nullptr) {
        send_tail_->next_ = std::addressof(waiter);
    } else {
        send_head_ = std::addressof(waiter);
    }
    send_tail_ = std::addressof(waiter);
}

template <typename t_type>
bool channel_state<t_type>::remove_send_waiter(channel_send_operation<t_type>& waiter) noexcept {
    if (!waiter.queued_) {
        return false;
    }
    if (waiter.previous_ != nullptr) {
        waiter.previous_->next_ = waiter.next_;
    } else {
        send_head_ = waiter.next_;
    }
    if (waiter.next_ != nullptr) {
        waiter.next_->previous_ = waiter.previous_;
    } else {
        send_tail_ = waiter.previous_;
    }
    waiter.previous_ = nullptr;
    waiter.next_ = nullptr;
    waiter.queued_ = false;
    return true;
}

template <typename t_type>
void channel_state<t_type>::grant_send_waiters() noexcept {
    while (send_head_ != nullptr && std::holds_alternative<channel_open>(lifecycle_)) {
        auto* slot = reserve_node();
        if (slot == nullptr) {
            return;
        }
        auto& waiter = *send_head_;
        (void)remove_send_waiter(waiter);
        waiter.complete_locked({channel_send_status::sent, slot});
    }
}

template <typename t_type>
void channel_state<t_type>::complete_send_waiters(channel_send_status status) noexcept {
    while (send_head_ != nullptr) {
        auto& waiter = *send_head_;
        (void)remove_send_waiter(waiter);
        waiter.complete_locked({status, nullptr});
    }
}

}  // namespace detail

template <typename t_type>
class channel_sender final {
public:
    channel_sender() noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

    // The sender must remain alive throughout the call, including payload
    // moves and destruction. Concurrent sends are delivered in publication
    // order; closure during a payload move returns that value as rejected.
    // Never reports timed_out or cancelled.
    [[nodiscard]] channel_send_result<t_type> send(t_type value) const {
        // Payload code may synchronously rebind this handle while the call is active.
        const auto state = state_;
        if (state == nullptr) {
            return detail::worker_delivery_access::reject<channel_send_result<t_type>>(channel_send_status::closed, std::move(value));
        }
        using state_type = detail::channel_state<t_type>;
        typename state_type::node* slot = nullptr;
        std::optional<channel_send_status> rejection;
        {
            auto lock = state->lock_state();
            rejection = state->check_open_locked();
            if (!rejection && (slot = state->reserve_node()) == nullptr) {
                rejection = channel_send_status::full;
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<channel_send_result<t_type>>(*rejection, std::move(value));
        }

        typename state_type::node_guard guard(*state, slot);
        try {
            slot->value_.emplace(std::move(value));
        } catch (...) {
            auto lock = state->lock_state();
            state->release_reserved(slot);
            guard.release();
            throw;
        }
        {
            auto lock = state->lock_state();
            rejection = state->publish_reserved(slot);
            if (!rejection) {
                guard.release();
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<channel_send_result<t_type>>(*rejection, std::move(*slot->value_));
        }
        return detail::worker_delivery_access::accept<channel_send_result<t_type>>();
    }

    // The waiting form of send(): a cold operation that, once awaited, takes a
    // free slot at once or suspends until the receiver releases one. Waiting
    // senders are served in FIFO order. It must be awaited on a worker thread,
    // which is where it resumes (std::logic_error elsewhere); the channel's
    // worker need not be that worker. A value that is not delivered -- the
    // channel closed or its worker stopped, the awaiting worker stopped
    // (worker_stopping), options.timeout_ elapsed (timed_out), or
    // options.stop_token_ fired (cancelled) -- returns in the result. Without
    // a timeout or stop token, the wait lasts until a slot frees, the channel
    // closes, or either worker stops.
    [[nodiscard]] channel_send_operation<t_type> send_wait(t_type value, operation_options options = {}) const& {
        return channel_send_operation<t_type>(state_, std::move(value), std::move(options));
    }
    channel_send_operation<t_type> send_wait(t_type, operation_options = {}) const&& = delete;

    // Stop sending while allowing the receiver to drain buffered values.
    void close() const {
        if (state_) {
            state_->close(false);
        }
    }

private:
    explicit channel_sender(std::shared_ptr<detail::channel_state<t_type>> state_value)
        : state_(std::move(state_value)) {}

    std::shared_ptr<detail::channel_state<t_type>> state_;
    template <typename u_type>
    friend auto make_channel(worker_ref, channel_options);
};

template <typename t_type>
class channel_receiver final {
public:
    channel_receiver() = delete;
    channel_receiver(const channel_receiver&) = delete;
    channel_receiver& operator=(const channel_receiver&) = delete;
    channel_receiver(channel_receiver&&) noexcept = default;
    channel_receiver& operator=(channel_receiver&&) = delete;

    ~channel_receiver() {
        close();
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }
    // The receive operation: single-use, movable only before it is awaited,
    // and started on the receiver's worker by co_await. Dropping a cold one
    // has no effect on the channel.
    class receive_operation final {
    public:
        receive_operation(receive_operation&&) noexcept = default;
        receive_operation(const receive_operation&) = delete;
        receive_operation& operator=(const receive_operation&) = delete;
        receive_operation& operator=(receive_operation&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready() {
            wait_.start("channel receive must run on its bound worker");
            auto& owner_value = wait_.state();
            auto lock = owner_value.lock_state();
            if (!owner_value.receiver_closed_) {
                if (auto* item = owner_value.dequeue()) {
                    owner_value.grant_send_waiters();
                    wait_.complete_ready(worker_wait_result<node*>::with_value(item));
                    return true;
                }
            }
            if (std::holds_alternative<detail::channel_worker_stopping>(owner_value.lifecycle_)) {
                wait_.complete_ready(worker_wait_status::worker_stopping);
                return true;
            }
            if (std::holds_alternative<detail::channel_closed>(owner_value.lifecycle_)) {
                wait_.complete_ready(worker_wait_status::closed);
                return true;
            }
            assert(std::holds_alternative<detail::channel_open>(owner_value.lifecycle_));
            return wait_.complete_or_publish("channel supports one pending receiver");
        }

        bool await_suspend(std::coroutine_handle<> continuation) {
            return wait_.suspend(continuation);
        }

        [[nodiscard]] worker_wait_result<t_type> await_resume() {
            auto result = wait_.take_result();
            if (!result.has_value()) {
                return worker_wait_result<t_type>::with_status(result.status());
            }
            auto* item = result.value();
            typename state_type::node_guard guard(wait_.state(), item);
            return worker_wait_result<t_type>::with_value(std::move(*item->value_));
        }

    private:
        friend class channel_receiver;
        using state_type = detail::channel_state<t_type>;
        using node = typename state_type::node;

        receive_operation(std::shared_ptr<state_type> state_value, operation_options options) noexcept
            : wait_(std::move(state_value), std::move(options)) {}

        worker_single_wait_awaiter<node*, state_type> wait_;
    };

    // The receiver owns the receive side: destroying it prevents cold receives
    // from consuming buffered values and closes pending receives. Require a
    // named owner so storing an operation cannot silently turn a temporary
    // receiver into an already-closed operation. A relative options.timeout_
    // bounds the wait from its start; a deadline that has passed reports
    // timed_out once nothing is buffered.
    [[nodiscard]] receive_operation receive(operation_options options = {}) const& {
        return receive_operation(state_, std::move(options));
    }
    receive_operation receive(operation_options = {}) const&& = delete;

    // The worker every receive must run on. Makes the receive-side affinity
    // contract queryable instead of only failing at await time. A moved-from
    // receiver has no bound worker and cannot answer this query.
    [[nodiscard]] const worker_ref& worker() const& noexcept {
        if (!state_) {
            std::terminate();
        }
        return state_->worker_;
    }
    const worker_ref& worker() const&& = delete;

    // Retire the receive side: buffered values are destroyed on the calling
    // thread and later receives report closed. Worker shutdown keeps its
    // worker_stopping outcome. The shared state itself lives until all
    // endpoints and tasks owning it retire.
    void close() const {
        if (state_) {
            // A buffered payload destructor may destroy this receiver and the
            // last sender, so the drain runs on its own owner of the state.
            const auto pinned = state_;
            pinned->close(true);
        }
    }

private:
    explicit channel_receiver(std::shared_ptr<detail::channel_state<t_type>> state_value)
        : state_(std::move(state_value)) {}

    std::shared_ptr<detail::channel_state<t_type>> state_;
    template <typename u_type>
    friend auto make_channel(worker_ref, channel_options);
};

template <typename t_type>
// The value parameter accepts both lvalue and rvalue worker refs before
// transferring the stable dispatcher endpoint into the channel state.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
[[nodiscard]] auto make_channel(worker_ref worker_value, channel_options options) {
    auto state_value = make_worker_wait_state<detail::channel_state<t_type>>(
        std::move(worker_value), options.capacity_);
    channel_sender<t_type> sender(state_value);
    return std::pair(std::move(sender), channel_receiver<t_type>(std::move(state_value)));
}

}  // namespace ruvia
