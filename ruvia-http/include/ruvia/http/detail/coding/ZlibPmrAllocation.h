#pragma once

#include <cstddef>
#include <memory_resource>

#include <zlib.h>

// zlib asks its caller for memory through two C callbacks. Both zlib users in
// this library route them to a PMR resource, which means every block must carry
// the resource and size needed to give it back -- zlib's free callback is handed
// only the pointer.

namespace ruvia::detail {

struct alignas(std::max_align_t) ZlibAllocationHeader final {
    std::pmr::memory_resource* resource;
    std::size_t bytes;
};

// Allocate `items * size` bytes from `resource`, stamped with the header the
// matching free needs. Returns nullptr on any overflow or allocation failure,
// which is how zlib expects an allocator to refuse.
[[nodiscard]] voidpf zlibPmrAllocate(std::pmr::memory_resource* resource, uInt items, uInt size) noexcept;

// Return a block obtained from zlibPmrAllocate to the resource it came from.
void zlibPmrFree(voidpf address) noexcept;

}  // namespace ruvia::detail
