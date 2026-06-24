#pragma once

#include "ruvia/memory/MemoryPool.h"

#include <cstddef>
#include <optional>
#include <span>

namespace ruvia::detail {

// Per-request arena initial block carried on the HTTP/2 dispatch coroutine frame.
// Sized to the shared kRequestArenaInitialBytes so the HTTP/1 work-set block and
// the HTTP/2 dispatch block start from one identical initial-block size.
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
