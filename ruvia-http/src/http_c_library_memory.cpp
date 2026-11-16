#include "http_c_library_memory.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace ruvia {

http_codec_memory::http_codec_memory(std::size_t max_bytes, http_codec_allocation_callbacks callbacks) :
    max_bytes_(max_bytes),
    callbacks_(callbacks) {
    if ((callbacks_.allocate_ == nullptr) != (callbacks_.deallocate_ == nullptr)) {
        throw std::invalid_argument("codec allocation callbacks must be supplied together");
    }
}

void* http_codec_memory::allocate(std::size_t payload_bytes, std::size_t total_bytes, std::size_t alignment) {
    if (payload_bytes > max_bytes_ - live_bytes_) {
        throw std::bad_alloc();
    }
    void* pointer = callbacks_.allocate_ != nullptr ? callbacks_.allocate_(total_bytes, alignment, callbacks_.context_) : ::operator new(total_bytes, std::align_val_t{alignment});
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    live_bytes_ += payload_bytes;
    return pointer;
}

void http_codec_memory::deallocate(void* pointer, std::size_t payload_bytes, std::size_t total_bytes, std::size_t alignment) noexcept {
    if (callbacks_.deallocate_ != nullptr) {
        callbacks_.deallocate_(pointer, total_bytes, alignment, callbacks_.context_);
    } else {
        ::operator delete(pointer, total_bytes, std::align_val_t{alignment});
    }
    live_bytes_ -= payload_bytes;
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

constexpr std::size_t allocation_alignment = alignof(std::max_align_t);
// The prefix keeps the payload max-aligned while recording the block size.
constexpr std::size_t allocation_prefix_bytes = ((sizeof(std::size_t) + allocation_alignment - 1) / allocation_alignment) * allocation_alignment;

[[nodiscard]] std::byte* allocation_base(void* pointer) noexcept {
    return static_cast<std::byte*>(pointer) - allocation_prefix_bytes;
}

[[nodiscard]] std::size_t allocation_payload_size(void* pointer) noexcept {
    std::size_t total = 0;
    std::memcpy(&total, allocation_base(pointer), sizeof(total));
    return total - allocation_prefix_bytes;
}

}  // namespace

http_c_library_memory::http_c_library_memory(std::size_t native_budget, bool throw_on_budget_exceeded, http_codec_memory* memory) noexcept :
    native_budget_(native_budget),
    throw_on_budget_exceeded_(throw_on_budget_exceeded),
    memory_(memory) {}

void http_c_library_memory::rethrow_if_failed() const {
    if (auto failure = std::exchange(failure_, nullptr)) {
        std::rethrow_exception(failure);
    }
}

void http_c_library_memory::rethrow_allocation_failure() const {
    rethrow_if_failed();
    throw std::bad_alloc();
}

void* http_c_library_memory::allocate(std::size_t size, void* context) noexcept {
    if (context == nullptr) {
        return nullptr;
    }
    auto& self = *static_cast<http_c_library_memory*>(context);
    if (size > self.native_budget_ - self.live_bytes_) {
        self.budget_exceeded_ = true;
        if (self.throw_on_budget_exceeded_ && !self.failure_) {
            self.failure_ = std::make_exception_ptr(std::bad_alloc{});
        }
        return nullptr;
    }
    if (size > (std::numeric_limits<std::size_t>::max)() - allocation_prefix_bytes) {
        return nullptr;
    }
    const auto total = size + allocation_prefix_bytes;
    try {
        auto* base = static_cast<std::byte*>(self.memory_ != nullptr ? self.memory_->allocate(size, total, allocation_alignment) : ::operator new(total, std::align_val_t{allocation_alignment}));
        std::memcpy(base, &total, sizeof(total));
        self.live_bytes_ += size;
        return base + allocation_prefix_bytes;
    } catch (...) {
        if (!self.failure_) {
            self.failure_ = std::current_exception();
        }
        return nullptr;
    }
}

void http_c_library_memory::deallocate(void* pointer, void* context) noexcept {
    if (pointer == nullptr) {
        return;
    }
    auto& self = *static_cast<http_c_library_memory*>(context);
    std::size_t total = 0;
    std::memcpy(&total, allocation_base(pointer), sizeof(total));
    if (self.memory_ != nullptr) {
        self.memory_->deallocate(allocation_base(pointer), total - allocation_prefix_bytes, total, allocation_alignment);
    } else {
        ::operator delete(allocation_base(pointer), total, std::align_val_t{allocation_alignment});
    }
    self.live_bytes_ -= total - allocation_prefix_bytes;
}

void* http_c_library_memory::allocate_zeroed(std::size_t count, std::size_t size, void* context) noexcept {
    if (size != 0 && count > (std::numeric_limits<std::size_t>::max)() / size) {
        return nullptr;
    }
    auto* pointer = allocate(count * size, context);
    if (pointer != nullptr) {
        std::memset(pointer, 0, count * size);
    }
    return pointer;
}

void* http_c_library_memory::reallocate(void* pointer, std::size_t size, void* context) noexcept {
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
