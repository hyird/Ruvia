#pragma once

#include <coroutine>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia::detail {

struct suspend_race_pending final {};

// A completion can race the suspension of the awaiter's coroutine in two
// orders: complete-then-suspend and suspend-then-complete. A caller that
// initiates an asynchronous operation must not resume the continuation from
// inside await_suspend before that coroutine is actually suspended; driving
// both orderings through this state machine makes the await_suspend return
// value (suspend or not) the single verdict on which ordering happened.
//
// Payload moves may throw; complete() rolls the state back to pending so the
// awaiter can retry or propagate the failure from the original call site.
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
