#pragma once

#include <chrono>
#include <cstdint>

namespace ruvia::detail {

// Monotonic milliseconds shared by worker-local deadlines (connection scanner,
// WebSocket liveness, tunnel drain, rate-limit windows). Values are comparable
// only with each other, never with wall-clock time.
[[nodiscard]] inline std::int64_t steady_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace ruvia::detail
