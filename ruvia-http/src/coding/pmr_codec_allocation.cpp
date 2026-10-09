#include "coding/pmr_codec_allocation.h"

#include <limits>
#include <memory>
#include <memory_resource>

namespace ruvia::detail {

namespace {

struct alignas(std::max_align_t) allocation_header final {
    std::pmr::memory_resource* resource_;
    std::size_t bytes_;
};

[[nodiscard]] void* allocate_codec_block(
    std::pmr::memory_resource* resource, std::size_t bytes_value) {
    if (resource == nullptr ||
        bytes_value > (std::numeric_limits<std::size_t>::max)() - sizeof(allocation_header)) {
        return nullptr;
    }
    const auto total_bytes = sizeof(allocation_header) + bytes_value;
    auto* raw = static_cast<std::byte*>(resource->allocate(total_bytes, alignof(allocation_header)));
    auto* header_value = std::construct_at(reinterpret_cast<allocation_header*>(raw), resource, total_bytes);
    return reinterpret_cast<std::byte*>(header_value) + sizeof(allocation_header);
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

void* pmr_codec_allocate(void* opaque, std::size_t bytes_value) noexcept {
    try {
        return allocate_codec_block(static_cast<std::pmr::memory_resource*>(opaque), bytes_value);
    } catch (...) {
        return nullptr;
    }
}

void* pmr_codec_allocate_with_exception(void* opaque, std::size_t bytes_value) noexcept {
    auto* context_value = static_cast<pmr_codec_allocation_context*>(opaque);
    if (context_value == nullptr) {
        return nullptr;
    }
    try {
        return allocate_codec_block(context_value->resource(), bytes_value);
    } catch (...) {
        context_value->capture_current_exception();
        return nullptr;
    }
}

void pmr_codec_free(void*, void* address) noexcept {
    if (address == nullptr) {
        return;
    }
    auto* raw = static_cast<std::byte*>(address) - sizeof(allocation_header);
    auto* header_value = reinterpret_cast<allocation_header*>(raw);
    auto* resource = header_value->resource_;
    const auto bytes_value = header_value->bytes_;
    std::destroy_at(header_value);
    resource->deallocate(raw, bytes_value, alignof(allocation_header));
}

}  // namespace ruvia::detail
