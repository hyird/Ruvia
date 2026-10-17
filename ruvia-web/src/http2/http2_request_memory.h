#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "ruvia/core/memory/memory_pool.h"

namespace ruvia::detail {

// Each concurrent HTTP/2 dispatch owns its arena block on its coroutine frame;
// it never borrows the HTTP/1 connection work-set's shared arena storage.
inline constexpr std::size_t http2_request_arena_stack_bytes = request_arena_initial_bytes;

inline request_memory& emplace_http2_request_memory(std::optional<request_memory>& storage,
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
