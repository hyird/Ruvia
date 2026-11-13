#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/stop_token.h"
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
