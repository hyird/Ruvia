#pragma once

#include <cstddef>
#include <cstdint>

namespace ruvia {

// Default initial bump-block size for a request arena: a request whose
// allocations fit within it touches no further heap. Runtime integrations that
// keep a reusable block per connection size it from the configured
// request_initial_buffer_bytes_, so a larger value stays free of per-request
// heap allocation at the cost of that much storage per active connection.
inline constexpr std::size_t request_arena_initial_bytes = std::size_t{4} * 1024;

// Default upper bound on the large blocks one worker_memory keeps cached for
// reuse after they are freed.
inline constexpr std::size_t default_retained_block_bytes = std::size_t{8} * 1024 * 1024;
// Largest accepted retained_block_bytes_: no address space can hold more.
inline constexpr std::size_t max_retained_block_bytes = static_cast<std::size_t>(PTRDIFF_MAX);

// request_initial_buffer_bytes_ must be greater than zero: it is the initial
// block size of every request arena.
// retained_block_bytes_ bounds the freed large blocks (above 16 KiB, up to
// 1 MiB, sizes rounded to 4 KiB) a worker keeps for exact-size reuse; blocks
// freed beyond it are returned immediately. Zero disables the cache; values
// above max_retained_block_bytes are rejected.
struct memory_pool_config {
    std::size_t request_initial_buffer_bytes_{request_arena_initial_bytes};
    std::size_t retained_block_bytes_{default_retained_block_bytes};
};

// The single validation used by worker_memory construction and by integrations
// that check startup configuration early. Throws std::invalid_argument.
void validate_memory_pool_config(const memory_pool_config& config);

}  // namespace ruvia
