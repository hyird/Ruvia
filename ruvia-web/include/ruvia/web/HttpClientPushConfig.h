#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
namespace ruvia {
struct HttpClientPushConfig final {
    bool enabled{false};
    std::size_t maxQueuedPushes{16};
    std::size_t maxConcurrentPushes{16};
    std::optional<std::chrono::milliseconds> timeout{30000};
};
}  // namespace ruvia
