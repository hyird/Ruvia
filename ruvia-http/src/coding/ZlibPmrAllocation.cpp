#include "ruvia/http/detail/coding/ZlibPmrAllocation.h"

#include <limits>

#include "ruvia/http/detail/coding/PmrCodecAllocation.h"

namespace ruvia::detail {

voidpf zlibPmrAllocate(std::pmr::memory_resource* resource, uInt items, uInt size) noexcept {
    if (resource == nullptr || items == 0 || size == 0) {
        return nullptr;
    }
    const auto itemBytes = static_cast<std::size_t>(items);
    const auto sizeBytes = static_cast<std::size_t>(size);
    if (itemBytes > (std::numeric_limits<std::size_t>::max)() / sizeBytes) {
        return nullptr;
    }
    const auto payloadBytes = itemBytes * sizeBytes;
    return pmrCodecAllocate(resource, payloadBytes);
}

void zlibPmrFree(voidpf address) noexcept {
    pmrCodecFree(nullptr, address);
}

}  // namespace ruvia::detail
