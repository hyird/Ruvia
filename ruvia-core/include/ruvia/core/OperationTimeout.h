#pragma once

#include <chrono>
#include <optional>

#include "ruvia/core/WorkerTimer.h"

namespace ruvia {

// Absolute timeout shared across the asynchronous phases of one operation.
class OperationTimeout final {
public:
    using Clock = std::chrono::steady_clock;

    explicit OperationTimeout(std::optional<std::chrono::milliseconds> timeout) noexcept {
        if (timeout.has_value()) {
            deadline_ = workerTimerDeadlineAfter(*timeout);
        }
    }

    [[nodiscard]] std::optional<std::chrono::milliseconds> remaining() const noexcept {
        if (!deadline_.has_value()) {
            return std::nullopt;
        }
        const auto now = Clock::now();
        if (now >= *deadline_) {
            return std::chrono::milliseconds(0);
        }
        return workerTimerCeilMilliseconds(*deadline_ - now);
    }

    [[nodiscard]] bool expired() const noexcept {
        const auto value = remaining();
        return value.has_value() && value->count() == 0;
    }

    [[nodiscard]] OperationTimeout constrainedBy(
        std::optional<std::chrono::milliseconds> timeout) const noexcept {
        OperationTimeout constrained(timeout);
        if (!deadline_.has_value()) {
            return constrained;
        }
        if (!constrained.deadline_.has_value() || *deadline_ < *constrained.deadline_) {
            constrained.deadline_ = deadline_;
        }
        return constrained;
    }

private:
    std::optional<Clock::time_point> deadline_;
};

}  // namespace ruvia

// Keep the generic deadline lifecycle available to runtime integrations that
// include this core capability header; OperationTimeout itself is public-owned.
#include "ruvia/core/detail/io/OperationDeadline.h"
