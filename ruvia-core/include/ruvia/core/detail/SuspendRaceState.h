#pragma once

#include <coroutine>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia::detail {

struct SuspendRacePending final {};

// A completion can race the suspension of the awaiter's coroutine in two
// orders: complete-then-suspend and suspend-then-complete. A caller that
// initiates an asynchronous operation must not resume the continuation from
// inside await_suspend before that coroutine is actually suspended; driving
// both orderings through this state machine makes the await_suspend return
// value (suspend or not) the single verdict on which ordering happened.
//
// Payload moves may throw; complete() rolls the state back to pending so the
// awaiter can retry or propagate the failure from the original call site.
template <typename T>
class SuspendRaceState final {
public:
    SuspendRaceState() = default;
    SuspendRaceState(const SuspendRaceState&) = delete;
    SuspendRaceState& operator=(const SuspendRaceState&) = delete;
    SuspendRaceState(SuspendRaceState&&) = delete;
    SuspendRaceState& operator=(SuspendRaceState&&) = delete;

    // Called from await_suspend after the operation was initiated: publishes
    // the continuation when still pending, or reports that the operation
    // already completed synchronously (its result is consumed by takeValue()).
    [[nodiscard]] bool suspend(std::coroutine_handle<> continuation) noexcept {
        if (std::holds_alternative<SuspendRacePending>(state_)) {
            state_.template emplace<SuspendRaceSuspended>(continuation);
            return true;
        }
        if (std::holds_alternative<SuspendRaceReadyBeforeSuspend>(state_)) {
            return false;
        }
        std::terminate();
    }

    // Returns true only when the suspended coroutine now needs an explicit
    // wake. A completion racing before await_suspend is consumed there instead.
    [[nodiscard]] bool complete(T&& value) {
        if (std::holds_alternative<SuspendRacePending>(state_)) {
            try {
                state_.template emplace<SuspendRaceReadyBeforeSuspend>(std::move(value));
            } catch (...) {
                state_.template emplace<SuspendRacePending>();
                throw;
            }
            return false;
        }
        if (auto* suspended = std::get_if<SuspendRaceSuspended>(&state_)) {
            const auto continuation = suspended->continuation();
            try {
                state_.template emplace<SuspendRaceReadyAfterSuspend>(
                    std::move(value), continuation);
            } catch (...) {
                state_.template emplace<SuspendRaceSuspended>(continuation);
                throw;
            }
            return true;
        }
        std::terminate();
    }

    [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
        const auto* ready = std::get_if<SuspendRaceReadyAfterSuspend>(&state_);
        if (ready == nullptr) {
            std::terminate();
        }
        return ready->continuation();
    }

    [[nodiscard]] T takeValue() noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (auto* ready = std::get_if<SuspendRaceReadyBeforeSuspend>(&state_)) {
            return std::move(*ready).takeValue();
        }
        if (auto* ready = std::get_if<SuspendRaceReadyAfterSuspend>(&state_)) {
            return std::move(*ready).takeValue();
        }
        std::terminate();
    }

private:
    class SuspendRaceSuspended final {
    public:
        explicit SuspendRaceSuspended(std::coroutine_handle<> continuation) noexcept
            : continuation_(continuation) {}

        [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
            return continuation_;
        }

    private:
        std::coroutine_handle<> continuation_;
    };

    class SuspendRaceReadyBeforeSuspend final {
    public:
        explicit SuspendRaceReadyBeforeSuspend(T&& value) noexcept(
            std::is_nothrow_move_constructible_v<T>)
            : value_(std::move(value)) {}

        [[nodiscard]] T takeValue() && noexcept(std::is_nothrow_move_constructible_v<T>) {
            return std::move(value_);
        }

    private:
        T value_;
    };

    class SuspendRaceReadyAfterSuspend final {
    public:
        SuspendRaceReadyAfterSuspend(T&& value, std::coroutine_handle<> continuation) noexcept(
            std::is_nothrow_move_constructible_v<T>)
            : value_(std::move(value)),
              continuation_(continuation) {}

        [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
            return continuation_;
        }

        [[nodiscard]] T takeValue() && noexcept(std::is_nothrow_move_constructible_v<T>) {
            return std::move(value_);
        }

    private:
        T value_;
        std::coroutine_handle<> continuation_;
    };

    using State = std::variant<SuspendRacePending, SuspendRaceSuspended,
        SuspendRaceReadyBeforeSuspend, SuspendRaceReadyAfterSuspend>;

    State state_;
};

}  // namespace ruvia::detail
