#include "http2/http2_nghttp2_memory.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace ruvia::detail {
namespace {

constexpr std::size_t allocation_alignment = alignof(std::max_align_t);
// The prefix keeps the payload max-aligned while recording the block size.
constexpr std::size_t allocation_prefix_bytes =
    ((sizeof(std::size_t) + allocation_alignment - 1) / allocation_alignment) * allocation_alignment;

[[nodiscard]] std::byte* allocation_base(void* pointer) noexcept {
    return static_cast<std::byte*>(pointer) - allocation_prefix_bytes;
}

[[nodiscard]] std::size_t allocation_payload_size(void* pointer) noexcept {
    std::size_t total = 0;
    std::memcpy(&total, allocation_base(pointer), sizeof(total));
    return total - allocation_prefix_bytes;
}

}  // namespace

http2_nghttp2_memory::http2_nghttp2_memory(std::pmr::memory_resource* resource) noexcept
    : resource_(resource),
      table_{.mem_user_data = this,
          .malloc = &http2_nghttp2_memory::allocate,
          .free = &http2_nghttp2_memory::deallocate,
          .calloc = &http2_nghttp2_memory::allocate_zeroed,
          .realloc = &http2_nghttp2_memory::reallocate} {}

void http2_nghttp2_memory::rethrow_allocation_failure() {
    if (auto failure = std::exchange(failure_, nullptr)) {
        std::rethrow_exception(failure);
    }
    throw std::bad_alloc();
}

void* http2_nghttp2_memory::allocate(std::size_t size, void* context) noexcept {
    auto& self = *static_cast<http2_nghttp2_memory*>(context);
    if (size > (std::numeric_limits<std::size_t>::max)() - allocation_prefix_bytes) {
        return nullptr;
    }
    const auto total = size + allocation_prefix_bytes;
    try {
        auto* base = static_cast<std::byte*>(self.resource_->allocate(total, allocation_alignment));
        std::memcpy(base, &total, sizeof(total));
        return base + allocation_prefix_bytes;
    } catch (...) {
        if (!self.failure_) {
            self.failure_ = std::current_exception();
        }
        return nullptr;
    }
}

void http2_nghttp2_memory::deallocate(void* pointer, void* context) noexcept {
    if (pointer == nullptr) {
        return;
    }
    auto& self = *static_cast<http2_nghttp2_memory*>(context);
    std::size_t total = 0;
    std::memcpy(&total, allocation_base(pointer), sizeof(total));
    self.resource_->deallocate(allocation_base(pointer), total, allocation_alignment);
}

void* http2_nghttp2_memory::allocate_zeroed(std::size_t count, std::size_t size, void* context) noexcept {
    if (size != 0 && count > (std::numeric_limits<std::size_t>::max)() / size) {
        return nullptr;
    }
    auto* pointer = allocate(count * size, context);
    if (pointer != nullptr) {
        std::memset(pointer, 0, count * size);
    }
    return pointer;
}

void* http2_nghttp2_memory::reallocate(void* pointer, std::size_t size, void* context) noexcept {
    if (pointer == nullptr) {
        return allocate(size, context);
    }
    if (size == 0) {
        deallocate(pointer, context);
        return nullptr;
    }
    auto* replacement = allocate(size, context);
    if (replacement == nullptr) {
        // C realloc semantics: the original block stays valid on failure.
        return nullptr;
    }
    std::memcpy(replacement, pointer, (std::min)(size, allocation_payload_size(pointer)));
    deallocate(pointer, context);
    return replacement;
}

}  // namespace ruvia::detail
