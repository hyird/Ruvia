#include "ruvia/http/detail/coding/PmrCodecAllocation.h"

#include <limits>
#include <memory>
#include <memory_resource>

namespace ruvia::detail {

namespace {

struct alignas(std::max_align_t) AllocationHeader final {
    std::pmr::memory_resource* resource;
    std::size_t bytes;
};

}  // namespace

void* pmrCodecAllocate(void* opaque, std::size_t bytes) noexcept {
    auto* resource = static_cast<std::pmr::memory_resource*>(opaque);
    if (resource == nullptr ||
        bytes > (std::numeric_limits<std::size_t>::max)() - sizeof(AllocationHeader)) {
        return nullptr;
    }
    const auto totalBytes = sizeof(AllocationHeader) + bytes;
    try {
        auto* raw = static_cast<std::byte*>(resource->allocate(totalBytes, alignof(AllocationHeader)));
        auto* header = std::construct_at(reinterpret_cast<AllocationHeader*>(raw), resource, totalBytes);
        return reinterpret_cast<std::byte*>(header) + sizeof(AllocationHeader);
    } catch (...) {
        return nullptr;
    }
}

void pmrCodecFree(void*, void* address) noexcept {
    if (address == nullptr) {
        return;
    }
    auto* raw = static_cast<std::byte*>(address) - sizeof(AllocationHeader);
    auto* header = reinterpret_cast<AllocationHeader*>(raw);
    auto* resource = header->resource;
    const auto bytes = header->bytes;
    std::destroy_at(header);
    resource->deallocate(raw, bytes, alignof(AllocationHeader));
}

}  // namespace ruvia::detail
