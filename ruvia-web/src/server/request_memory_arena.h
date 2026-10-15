#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "ruvia/core/memory/memory_pool.h"

namespace ruvia::detail {

// Where one request's arena starts. Both session drivers call
// emplace_request_memory: HTTP/1 hands it the work-set block it borrowed for the
// connection, HTTP/2 hands it a block on the dispatch coroutine's own frame,
// sized by request_arena_stack_bytes below. Both start from the same
// request_arena_initial_bytes, so a request costs the same either way.
inline constexpr std::size_t request_arena_stack_bytes = request_arena_initial_bytes;

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
