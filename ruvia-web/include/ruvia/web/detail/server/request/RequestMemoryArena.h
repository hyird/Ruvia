#pragma once

#include "ruvia/core/memory/MemoryPool.h"

#include <cstddef>
#include <optional>
#include <span>

namespace ruvia::detail {

// Where one request's arena starts. Both session drivers call
// emplaceRequestMemory: HTTP/1 hands it the work-set block it borrowed for the
// connection, HTTP/2 hands it a block on the dispatch coroutine's own frame,
// sized by kRequestArenaStackBytes below. Both start from the same
// kRequestArenaInitialBytes, so a request costs the same either way.
inline constexpr std::size_t kRequestArenaStackBytes = kRequestArenaInitialBytes;

inline RequestMemory& emplaceRequestMemory(
    std::optional<RequestMemory>& storage,
    WorkerMemory& memory,
    std::span<std::byte> initialBuffer) {
    const auto initialBytes = memory.requestInitialBufferBytes();
    if (initialBytes <= initialBuffer.size()) {
        storage.emplace(memory, initialBuffer.first(initialBytes));
    } else {
        storage.emplace(memory);
    }
    return *storage;
}

}  // namespace ruvia::detail
