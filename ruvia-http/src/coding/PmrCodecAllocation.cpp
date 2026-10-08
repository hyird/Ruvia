#include "coding/PmrCodecAllocation.h"

#include <limits>
#include <memory>
#include <memory_resource>

namespace ruvia::detail {

namespace {

struct alignas(std::max_align_t) AllocationHeader final {
    std::pmr::memory_resource* resource;
    std::size_t bytes;
};

[[nodiscard]] void* allocateCodecBlock(
    std::pmr::memory_resource* resource, std::size_t bytes) {
    if (resource == nullptr ||
        bytes > (std::numeric_limits<std::size_t>::max)() - sizeof(AllocationHeader)) {
        return nullptr;
    }
    const auto totalBytes = sizeof(AllocationHeader) + bytes;
    auto* raw = static_cast<std::byte*>(resource->allocate(totalBytes, alignof(AllocationHeader)));
    auto* header = std::construct_at(reinterpret_cast<AllocationHeader*>(raw), resource, totalBytes);
    return reinterpret_cast<std::byte*>(header) + sizeof(AllocationHeader);
}

}  // namespace

void pmr_codec_allocation_context::capture_current_exception() noexcept {
    if (failure_ == nullptr) {
        failure_ = std::current_exception();
    }
}

void pmr_codec_allocation_context::rethrow_allocation_failure() const {
    if (failure_ != nullptr) {
        std::rethrow_exception(failure_);
    }
}

void* pmrCodecAllocate(void* opaque, std::size_t bytes) noexcept {
    try {
        return allocateCodecBlock(static_cast<std::pmr::memory_resource*>(opaque), bytes);
    } catch (...) {
        return nullptr;
    }
}

void* pmr_codec_allocate_with_exception(void* opaque, std::size_t bytes) noexcept {
    auto* context = static_cast<pmr_codec_allocation_context*>(opaque);
    if (context == nullptr) {
        return nullptr;
    }
    try {
        return allocateCodecBlock(context->resource(), bytes);
    } catch (...) {
        context->capture_current_exception();
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
