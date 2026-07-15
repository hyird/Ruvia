#pragma once

#include <cstdint>

namespace ruvia::detail {

enum class HttpServerWorkerState : std::uint8_t {
    kFresh,
    kRunning,
    kDraining,
    kStopped,
};

[[nodiscard]] inline bool httpServerWorkerRunning(
    HttpServerWorkerState state) noexcept {
    return state == HttpServerWorkerState::kRunning;
}

}  // namespace ruvia::detail
