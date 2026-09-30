#pragma once

#include <cstddef>

namespace ruvia {

// Default initial bump-block size for a request arena. Runtime integrations size
// their connection-private dispatch blocks to this same constant, so configured
// defaults and compile-time blocks stay in lockstep: a request whose allocations
// fit within it touches no heap at all.
//
// Configuring requestInitialBufferBytes larger than this constant stays correct
// but spills the arena's first block to the worker resource on every request,
// because the in-block fast path is sized at compile time. Prefer raising
// kRequestArenaInitialBytes itself if a larger zero-heap default is wanted.
inline constexpr std::size_t kRequestArenaInitialBytes = std::size_t{4} * 1024;

struct MemoryPoolConfig {
    std::size_t requestInitialBufferBytes{kRequestArenaInitialBytes};
};

}  // namespace ruvia
