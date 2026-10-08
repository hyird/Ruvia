#include "coding/ZlibPmrAllocation.h"

#include <limits>

#include "coding/PmrCodecAllocation.h"

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

voidpf zlib_pmr_allocate_with_exception(void* context, uInt items, uInt size) noexcept {
    if (context == nullptr || items == 0 || size == 0) {
        return nullptr;
    }
    const auto itemBytes = static_cast<std::size_t>(items);
    const auto sizeBytes = static_cast<std::size_t>(size);
    if (itemBytes > (std::numeric_limits<std::size_t>::max)() / sizeBytes) {
        return nullptr;
    }
    const auto payloadBytes = itemBytes * sizeBytes;
    return pmr_codec_allocate_with_exception(context, payloadBytes);
}

void zlibPmrFree(voidpf address) noexcept {
    pmrCodecFree(nullptr, address);
}

}  // namespace ruvia::detail
