#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
namespace ruvia {
struct http_client_push_config final {
    bool enabled_{false};
    std::size_t max_queued_pushes_{16};
    std::size_t max_concurrent_pushes_{16};
    std::optional<std::chrono::milliseconds> timeout_{30000};
};
}  // namespace ruvia
