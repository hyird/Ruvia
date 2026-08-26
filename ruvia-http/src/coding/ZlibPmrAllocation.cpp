#include "ruvia/http/detail/coding/ZlibPmrAllocation.h"
#include "ruvia/http/detail/coding/PmrCodecAllocation.h"

#include <limits>
#include <new>

namespace ruvia::detail {

void* pmrCodecAllocate(void* opaque, std::size_t bytes) noexcept {
    auto* resource = static_cast<std::pmr::memory_resource*>(opaque);
    if (resource == nullptr ||
        bytes > (std::numeric_limits<std::size_t>::max)() - sizeof(PmrCodecAllocationHeader)) {
        return nullptr;
    }
    const auto totalBytes = sizeof(PmrCodecAllocationHeader) + bytes;
    try {
        auto* raw = static_cast<std::byte*>(
            resource->allocate(totalBytes, alignof(PmrCodecAllocationHeader)));
        auto* header = reinterpret_cast<PmrCodecAllocationHeader*>(raw);
        header->resource = resource;
        header->bytes = totalBytes;
        return raw + sizeof(PmrCodecAllocationHeader);
    } catch (...) {
        return nullptr;
    }
}

void pmrCodecFree(void*, void* address) noexcept {
    if (address == nullptr) {
        return;
    }
    auto* raw = static_cast<std::byte*>(address) - sizeof(PmrCodecAllocationHeader);
    auto* header = reinterpret_cast<PmrCodecAllocationHeader*>(raw);
    header->resource->deallocate(raw, header->bytes, alignof(PmrCodecAllocationHeader));
}

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
    if (payloadBytes > (std::numeric_limits<std::size_t>::max)() - sizeof(ZlibAllocationHeader)) {
        return nullptr;
    }
    const auto totalBytes = sizeof(ZlibAllocationHeader) + payloadBytes;
    try {
        auto* raw =
            static_cast<std::byte*>(resource->allocate(totalBytes, alignof(ZlibAllocationHeader)));
        auto* header = reinterpret_cast<ZlibAllocationHeader*>(raw);
        header->resource = resource;
        header->bytes = totalBytes;
        return raw + sizeof(ZlibAllocationHeader);
    } catch (...) {
        return nullptr;
    }
}

void zlibPmrFree(voidpf address) noexcept {
    if (address == nullptr) {
        return;
    }
    auto* raw = static_cast<std::byte*>(address) - sizeof(ZlibAllocationHeader);
    auto* header = reinterpret_cast<ZlibAllocationHeader*>(raw);
    header->resource->deallocate(raw, header->bytes, alignof(ZlibAllocationHeader));
}

}  // namespace ruvia::detail
