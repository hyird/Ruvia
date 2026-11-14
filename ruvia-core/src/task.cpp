#include "task.h"

#include <algorithm>
#include <cstddef>
#include <new>

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

namespace ruvia {

namespace {

[[nodiscard]] void* allocate_mimalloc(std::size_t bytes_value, std::size_t alignment) {
#if RUVIA_SANITIZER_ALLOCATOR
    // The system allocators may answer a zero-byte request with null.
    const std::size_t size = bytes_value == 0 ? 1 : bytes_value;
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
    // mimalloc returns a unique, aligned block for zero bytes.
    if (void* block = mi_malloc_aligned(bytes_value, alignment)) {
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

}  // namespace

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

}  // namespace ruvia
