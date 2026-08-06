#pragma once

#include <cstddef>
#include <memory_resource>

// Brotli and zstd pass only the allocation pointer to their free callback.
// Stamp each block with its originating PMR resource and total allocation size
// so codec state follows the same ownership boundary as protocol output.

namespace ruvia::detail {

struct alignas(std::max_align_t) PmrCodecAllocationHeader final {
    std::pmr::memory_resource* resource;
    std::size_t bytes;
};

[[nodiscard]] void* pmrCodecAllocate(void* opaque, std::size_t bytes) noexcept;
void pmrCodecFree(void* opaque, void* address) noexcept;

}  // namespace ruvia::detail
