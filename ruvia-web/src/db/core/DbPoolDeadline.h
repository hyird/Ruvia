#pragma once

#include "../DbInternal.h"

#include <chrono>

namespace ruvia {

struct detail::MariaDbPool::OperationDeadline final {
    std::chrono::milliseconds fallbackTimeout{0};
    std::chrono::steady_clock::time_point deadline{};
    bool hasDeadline{false};

    explicit OperationDeadline(std::chrono::milliseconds timeout) noexcept
        : fallbackTimeout(timeout),
          deadline(std::chrono::steady_clock::now() + timeout),
          hasDeadline(timeout.count() > 0) {}

    [[nodiscard]] std::chrono::milliseconds remaining() const noexcept {
        if (!hasDeadline) {
            return fallbackTimeout;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::chrono::milliseconds(0);
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    }
};

}  // namespace ruvia
