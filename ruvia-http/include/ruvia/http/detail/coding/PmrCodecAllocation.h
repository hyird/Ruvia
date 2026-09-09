#pragma once

#include <cstddef>

// Codec free callbacks need not retain the allocating resource themselves.
// The implementation stamps each block with its originating PMR resource and
// total allocation size so codec state follows the same ownership boundary as
// protocol output.

namespace ruvia::detail {

[[nodiscard]] void* pmrCodecAllocate(void* opaque, std::size_t bytes) noexcept;
void pmrCodecFree(void* opaque, void* address) noexcept;

}  // namespace ruvia::detail
