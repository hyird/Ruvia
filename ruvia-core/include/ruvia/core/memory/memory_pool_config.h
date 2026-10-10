#pragma once

#include <cstddef>

namespace ruvia {

// Default initial bump-block size for a request arena. Runtime integrations size
// their connection-private dispatch blocks to this same constant, so configured
// defaults and compile-time blocks stay in lockstep: a request whose allocations
// fit within it touches no heap at all.
//
// Configuring request_initial_buffer_bytes larger than this constant stays correct
// but spills the arena's first block to the worker resource on every request,
// because the in-block fast path is sized at compile time. Prefer raising
// request_arena_initial_bytes itself if a larger zero-heap default is wanted.
inline constexpr std::size_t request_arena_initial_bytes = std::size_t{4} * 1024;

// request_initial_buffer_bytes_ must be greater than zero: it is the initial
// block size of every request arena.
struct memory_pool_config {
    std::size_t request_initial_buffer_bytes_{request_arena_initial_bytes};
};

// The single validation used by worker_memory construction and by integrations
// that check startup configuration early. Throws std::invalid_argument.
void validate_memory_pool_config(const memory_pool_config& config);

}  // namespace ruvia
