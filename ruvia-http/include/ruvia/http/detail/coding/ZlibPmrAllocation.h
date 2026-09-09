#pragma once

#include <zlib.h>

#include <memory_resource>

// zlib asks its caller for memory through two C callbacks. These adapters route
// non-empty allocations to the common codec PMR allocator; zlib's free callback
// is handed only the pointer.

namespace ruvia::detail {

// Allocate `items * size` bytes from `resource`. Returns nullptr for zero-sized
// requests, overflow, or allocation failure, which is how zlib expects refusal.
[[nodiscard]] voidpf zlibPmrAllocate(
    std::pmr::memory_resource* resource, uInt items, uInt size) noexcept;

// Return a block obtained from zlibPmrAllocate to the resource it came from.
void zlibPmrFree(voidpf address) noexcept;

}  // namespace ruvia::detail
