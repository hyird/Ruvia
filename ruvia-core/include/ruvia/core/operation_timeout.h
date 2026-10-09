#pragma once

#include <chrono>
#include <optional>

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
        const auto value = remaining();
        return value.has_value() && value->count() == 0;
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

}  // namespace ruvia
