#pragma once

#include "ruvia/web/detail/db/DbInternal.h"

#include <chrono>
#include <optional>

namespace ruvia {

struct detail::MariaDbPool::OperationDeadline final {
    std::optional<std::chrono::steady_clock::time_point> deadline;

    explicit OperationDeadline(
        std::optional<std::chrono::milliseconds> timeout) noexcept {
        if (timeout.has_value()) {
            deadline = std::chrono::steady_clock::now() + *timeout;
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

}  // namespace ruvia
