#pragma once

#include "ruvia/memory/MemoryPool.h"

#include <cstddef>
#include <optional>
#include <span>

namespace ruvia::detail {

inline constexpr std::size_t kRequestArenaStackBytes = 4 * 1024;

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
