#pragma once

#include <chrono>
#include <optional>

#include "ruvia/core/detail/WorkerTimer.h"

namespace ruvia::detail {

class DbOperationDeadline final {
public:
    std::optional<std::chrono::steady_clock::time_point> deadline;

    explicit DbOperationDeadline(
        std::optional<std::chrono::milliseconds> timeout) noexcept {
        if (timeout.has_value()) {
            deadline = workerTimerDeadlineAfter(*timeout);
        }
    }

    [[nodiscard]] std::optional<std::chrono::milliseconds> remaining() const noexcept {
        if (!deadline.has_value()) {
            return std::nullopt;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= *deadline) {
            return std::chrono::milliseconds(0);
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
    }
};

}  // namespace ruvia::detail
