#pragma once

#include <chrono>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/worker_timer.h"

namespace ruvia {

// Absolute timeout shared across the asynchronous phases of one operation.
class operation_timeout final {
public:
    using clock_type = std::chrono::steady_clock;

    explicit operation_timeout(std::optional<std::chrono::milliseconds> timeout) noexcept {
        if (timeout.has_value()) {
            deadline_ = worker_timer_deadline_after(*timeout);
        }
    }

    [[nodiscard]] std::optional<clock_type::time_point> deadline() const noexcept {
        return deadline_;
    }

    [[nodiscard]] std::optional<std::chrono::milliseconds> remaining() const noexcept {
        if (!deadline_.has_value()) {
            return std::nullopt;
        }
        const auto now = clock_type::now();
        if (now >= *deadline_) {
            return std::chrono::milliseconds(0);
        }
        return worker_timer_ceil_milliseconds(*deadline_ - now);
    }

    [[nodiscard]] bool expired() const noexcept {
        return deadline_.has_value() && clock_type::now() >= *deadline_;
    }

    [[nodiscard]] operation_timeout constrained_by(
        std::optional<std::chrono::milliseconds> timeout) const noexcept {
        operation_timeout constrained(timeout);
        if (!deadline_.has_value()) {
            return constrained;
        }
        if (!constrained.deadline_.has_value() || *deadline_ < *constrained.deadline_) {
            constrained.deadline_ = deadline_;
        }
        return constrained;
    }

private:
    std::optional<clock_type::time_point> deadline_;
};

// One allocation-free deadline lifecycle. A deadline is either absent, armed
// with its cancellation kind and exact time point, or expired with that same
// kind retained until the awaiting operation consumes the outcome.
template <typename kind_type = std::monostate>
    requires(std::is_enum_v<kind_type> || std::is_same_v<kind_type, std::monostate>)
class operation_deadline final {
public:
    using clock_type = std::chrono::steady_clock;

    void arm(clock_type::time_point deadline_value, kind_type kind) noexcept {
        state_.template emplace<active>(deadline_value, std::move(kind));
    }

    void arm(clock_type::time_point deadline_value) noexcept
        requires std::is_same_v<kind_type, std::monostate>
    {
        arm(deadline_value, std::monostate{});
    }

    void arm(std::optional<std::chrono::milliseconds> timeout) noexcept
        requires std::is_same_v<kind_type, std::monostate>
    {
        if (timeout.has_value()) {
            arm(worker_timer_deadline_after(*timeout), std::monostate{});
        } else {
            reset();
        }
    }
    void reset() noexcept {
        state_.template emplace<inactive>();
    }

    [[nodiscard]] bool expired() const noexcept {
        return std::holds_alternative<expired_state>(state_);
    }

    [[nodiscard]] const kind_type* kind() const& noexcept {
        if (const auto* armed = std::get_if<active>(&state_)) {
            return &armed->kind_;
        }
        if (const auto* expired_value = std::get_if<expired_state>(&state_)) {
            return &expired_value->kind_;
        }
        return nullptr;
    }
    const kind_type* kind() const&& = delete;

    [[nodiscard]] std::optional<kind_type> expire(clock_type::time_point now) noexcept {
        const auto* armed = std::get_if<active>(&state_);
        if (armed == nullptr || armed->deadline_ > now) {
            return std::nullopt;
        }
        auto kind = armed->kind_;
        state_.template emplace<expired_state>(kind);
        return kind;
    }

    [[nodiscard]] bool clear() noexcept {
        const bool was_expired = expired();
        reset();
        return was_expired;
    }

private:
    struct inactive final {};

    struct active final {
        active(clock_type::time_point armed_deadline, kind_type armed_kind) noexcept
            : deadline_(armed_deadline),
              kind_(std::move(armed_kind)) {}

        clock_type::time_point deadline_;
        kind_type kind_;
    };

    struct expired_state final {
        explicit expired_state(kind_type expired_kind) noexcept
            : kind_(std::move(expired_kind)) {}

        kind_type kind_;
    };

    using state_type = std::variant<inactive, active, expired_state>;

    state_type state_;
};

}  // namespace ruvia
