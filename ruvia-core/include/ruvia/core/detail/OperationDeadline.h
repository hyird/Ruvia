#pragma once

#include <chrono>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia::detail {

// One allocation-free deadline lifecycle. A deadline is either absent, armed
// with its cancellation kind and exact time point, or expired with that same
// kind retained until the awaiting operation consumes the outcome.
template <typename Kind>
    requires std::is_enum_v<Kind>
class OperationDeadline final {
public:
    using Clock = std::chrono::steady_clock;

    void arm(Clock::time_point deadline, Kind kind) noexcept {
        state_.template emplace<Active>(deadline, std::move(kind));
    }

    void reset() noexcept {
        state_.template emplace<Inactive>();
    }

    [[nodiscard]] bool expired() const noexcept {
        return std::holds_alternative<Expired>(state_);
    }

    [[nodiscard]] const Kind* kind() const & noexcept {
        if (const auto* active = std::get_if<Active>(&state_)) {
            return &active->kind;
        }
        if (const auto* expiredState = std::get_if<Expired>(&state_)) {
            return &expiredState->kind;
        }
        return nullptr;
    }
    const Kind* kind() const && = delete;

    [[nodiscard]] std::optional<Kind> expire(Clock::time_point now) noexcept {
        const auto* active = std::get_if<Active>(&state_);
        if (active == nullptr || active->deadline > now) {
            return std::nullopt;
        }
        auto kind = active->kind;
        state_.template emplace<Expired>(kind);
        return kind;
    }

    [[nodiscard]] bool clear() noexcept {
        const bool wasExpired = expired();
        reset();
        return wasExpired;
    }

private:
    struct Inactive final {};

    struct Active final {
        Active(Clock::time_point armedDeadline, Kind armedKind) noexcept
            : deadline(armedDeadline), kind(std::move(armedKind)) {}

        Clock::time_point deadline;
        Kind kind;
    };

    struct Expired final {
        explicit Expired(Kind expiredKind) noexcept
            : kind(std::move(expiredKind)) {}

        Kind kind;
    };

    using State = std::variant<Inactive, Active, Expired>;

    State state_;
};

}  // namespace ruvia::detail
