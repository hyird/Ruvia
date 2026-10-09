#include "coding/zlib_pmr_allocation.h"

#include <limits>

#include "coding/pmr_codec_allocation.h"

namespace ruvia::detail {

voidpf zlib_pmr_allocate(std::pmr::memory_resource* resource, uInt items, uInt size) noexcept {
    if (resource == nullptr || items == 0 || size == 0) {
        return nullptr;
    }
    const auto item_bytes = static_cast<std::size_t>(items);
    const auto size_bytes = static_cast<std::size_t>(size);
    if (item_bytes > (std::numeric_limits<std::size_t>::max)() / size_bytes) {
        return nullptr;
    }
    const auto payload_bytes = item_bytes * size_bytes;
    return pmr_codec_allocate(resource, payload_bytes);
}

voidpf zlib_pmr_allocate_with_exception(void* context_value, uInt items, uInt size) noexcept {
    if (context_value == nullptr || items == 0 || size == 0) {
        return nullptr;
    }
    const auto item_bytes = static_cast<std::size_t>(items);
    const auto size_bytes = static_cast<std::size_t>(size);
    if (item_bytes > (std::numeric_limits<std::size_t>::max)() / size_bytes) {
        return nullptr;
    }
    const auto payload_bytes = item_bytes * size_bytes;
    return pmr_codec_allocate_with_exception(context_value, payload_bytes);
}

void zlib_pmr_free(voidpf address) noexcept {
    pmr_codec_free(nullptr, address);
}

}  // namespace ruvia::detail
