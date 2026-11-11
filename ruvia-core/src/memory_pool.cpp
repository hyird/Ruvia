#include "ruvia/core/memory/memory_pool.h"

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
#include "ruvia/core/worker_handle.h"

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

void validate_memory_pool_config(const memory_pool_config& config) {
    if (config.request_initial_buffer_bytes_ == 0) {
        throw std::invalid_argument("memory pool request initial buffer size must be greater than zero");
    }
}

worker_memory::worker_memory(const memory_pool_config& config)
    : request_initial_buffer_bytes_(config.request_initial_buffer_bytes_) {
    validate_memory_pool_config(config);
#if !RUVIA_SANITIZER_ALLOCATOR
    heap_ = mi_heap_new();
    if (heap_ == nullptr) {
        throw std::bad_alloc();
    }
#endif
}

worker_memory::~worker_memory() {
#if !RUVIA_SANITIZER_ALLOCATOR
    mi_heap_delete(heap_);
#endif
}

void* worker_memory::do_allocate(std::size_t bytes_value, std::size_t alignment) {
#if RUVIA_SANITIZER_ALLOCATOR
    return allocate_mimalloc(bytes_value, alignment);
#else
    if (void* block = mi_heap_malloc_aligned(heap_, bytes_value == 0 ? 1 : bytes_value, alignment)) {
        return block;
    }
    throw std::bad_alloc();
#endif
}

void worker_memory::do_deallocate(void* block, std::size_t, std::size_t) noexcept {
    free_mimalloc(block);
}

bool worker_memory::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

void worker_memory::trim(const worker_handle& owner) {
    const auto owner_id = owner.id();
    if (owner_id == 0 || !owner.is_current() || (owner_id_ != 0 && owner_id != owner_id_)) {
        throw std::logic_error("worker memory trim must run on its owner worker");
    }
    owner_id_ = owner_id;
#if !RUVIA_SANITIZER_ALLOCATOR
    // Default-heap frames follow mimalloc's normal page retirement policy.
    // Only the final worker collection forces the shared arenas to purge.
    mi_collect(false);
    mi_heap_collect(heap_, true);
#endif
}

request_memory::request_memory(worker_memory& worker_value)
    : request_memory(worker_value.request_initial_buffer_bytes(), worker_value.resource()) {}

request_memory::request_memory(worker_memory& worker_value, std::span<std::byte> initial_buffer)
    : arena_(initial_buffer.data(), initial_buffer.size(), worker_value.resource()),
      initial_bytes_(worker_value.request_initial_buffer_bytes()) {}

request_memory::request_memory(std::size_t initial_bytes, std::pmr::memory_resource* upstream)
    : arena_(initial_bytes, upstream),
      initial_bytes_(initial_bytes) {}

request_memory request_memory::fork() const& {
    return request_memory(initial_bytes_, arena_.upstream_resource());
}

std::pmr::memory_resource* request_memory::resource() & noexcept {
    return &arena_;
}

std::pmr::memory_resource* request_memory::resource() const& noexcept {
    return &arena_;
}

std::pmr::memory_resource* request_memory::upstream_resource() & noexcept {
    return arena_.upstream_resource();
}

std::pmr::memory_resource* request_memory::upstream_resource() const& noexcept {
    return arena_.upstream_resource();
}

}  // namespace ruvia
