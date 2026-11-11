#include "ruvia/core/memory/request_memory.h"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define RUVIA_SANITIZER_ALLOCATOR 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define RUVIA_SANITIZER_ALLOCATOR 1
#else
#define RUVIA_SANITIZER_ALLOCATOR 0
#endif
#else
#define RUVIA_SANITIZER_ALLOCATOR 0
#endif

#if RUVIA_SANITIZER_ALLOCATOR
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
#endif
#else
#include <mimalloc.h>
#endif

#include <algorithm>
#include <memory_resource>
#include <new>
#include <stdexcept>

#include "ruvia/core/detail/task/task_promise.h"
#include "ruvia/core/memory/pmr_object.h"

namespace ruvia {

namespace {

[[nodiscard]] void* allocate_mimalloc(std::size_t bytes_value, std::size_t alignment) {
    const std::size_t size = bytes_value == 0 ? 1 : bytes_value;
#if RUVIA_SANITIZER_ALLOCATOR
#if defined(_WIN32)
    if (void* block = _aligned_malloc(size, (std::max)(alignment, alignof(void*)))) {
        return block;
    }
#else
    void* block = nullptr;
    if (::posix_memalign(&block, (std::max)(alignment, alignof(void*)), size) == 0 && block != nullptr) {
        return block;
    }
#endif
#else
    if (void* block = mi_malloc_aligned(size, alignment)) {
        return block;
    }
#endif
    throw std::bad_alloc();
}

void free_mimalloc(void* block) noexcept {
#if RUVIA_SANITIZER_ALLOCATOR
#if defined(_WIN32)
    _aligned_free(block);
#else
    std::free(block);
#endif
#else
    mi_free(block);
#endif
}

class mimalloc_memory_resource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        return allocate_mimalloc(bytes_value, alignment);
    }

    void do_deallocate(void* block, std::size_t, std::size_t) noexcept override {
        free_mimalloc(block);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace

std::pmr::memory_resource* process_resource() noexcept {
    // The adapter has process lifetime without a C++ static destructor:
    // detached blocking work may release completion state during teardown.
    // mimalloc owns its thread heaps and supports freeing on another thread,
    // including after the allocating thread exits. No shared pool is layered
    // over it, and the default PMR resource object is not replaced.
    alignas(mimalloc_memory_resource) static std::byte storage[sizeof(mimalloc_memory_resource)];
    static auto* const resource = ::new (storage) mimalloc_memory_resource;
    return resource;
}

namespace detail {

void* task_frame_allocate(std::size_t bytes_value) {
    return allocate_mimalloc(bytes_value, alignof(std::max_align_t));
}

void task_frame_deallocate(void* pointer) noexcept {
    free_mimalloc(pointer);
}

void* task_frame_allocate_aligned(std::size_t bytes_value, std::size_t alignment) {
    return allocate_mimalloc(bytes_value, alignment);
}

}  // namespace detail

void collect_thread_heap() noexcept {
#if !RUVIA_SANITIZER_ALLOCATOR
    // mi_collect() serves the calling thread's heap; forcing also purges the
    // shared arenas.
    mi_collect(true);
#endif
}

namespace {

[[nodiscard]] std::size_t validated_request_initial_bytes(std::size_t initial_bytes) {
    if (initial_bytes == 0) {
        throw std::invalid_argument("request arena initial buffer size must be greater than zero");
    }
    return initial_bytes;
}

}  // namespace

request_memory::request_memory(std::size_t initial_bytes)
    : arena_(validated_request_initial_bytes(initial_bytes), process_resource()),
      initial_bytes_(initial_bytes) {}

request_memory::request_memory(std::size_t initial_bytes, std::span<std::byte> initial_buffer)
    : arena_(initial_buffer.data(), initial_buffer.size(), process_resource()),
      initial_bytes_(validated_request_initial_bytes(initial_bytes)) {}

request_memory request_memory::fork() const& {
    return request_memory(initial_bytes_);
}

std::pmr::memory_resource* request_memory::resource() & noexcept {
    return &arena_;
}

std::pmr::memory_resource* request_memory::resource() const& noexcept {
    return &arena_;
}

}  // namespace ruvia
