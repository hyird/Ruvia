#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "ruvia/core/memory/memory_pool.h"

namespace ruvia::detail {

// Start each request from the reusable connection work-set buffer when it fits.
inline request_memory& emplace_request_memory(std::optional<request_memory>& storage,
    worker_memory& memory, std::span<std::byte> initial_buffer) {
    const auto initial_bytes = memory.request_initial_buffer_bytes();
    if (initial_bytes <= initial_buffer.size()) {
        storage.emplace(memory, initial_buffer.first(initial_bytes));
    } else {
        storage.emplace(memory);
    }
    return *storage;
}

}  // namespace ruvia::detail
