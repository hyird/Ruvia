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
#else
#include <mimalloc.h>
#endif

#include <algorithm>
#include <new>
#include <stdexcept>

#include "ruvia/core/detail/task/task_promise.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_handle.h"

#include "size_class_lifo_bins.h"
#include "worker_block_cache.h"

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

namespace {

// Coroutine frames come and go several times per request, always in a handful
// of recurring sizes. A thread-local LIFO cache keyed by 128-byte size class
// hands a just-freed frame straight to the next allocation of the same class,
// so the request hot path skips the general allocator and reuses cache-warm
// memory. Blocks are allocated at the class ceiling, which keeps every cached
// block large enough for any request that maps to its bin. Deallocation is
// always sized, so every freed frame maps back to its class.
using task_frame_bins = size_class_lifo_bins<128, 16 * 1024>;
constexpr std::size_t task_frame_cache_budget_bytes = 512 * 1024;

[[nodiscard]] constexpr std::size_t task_frame_class_bytes(std::size_t bytes_value) noexcept {
    // Large frames bypass the cache; rounding them could overflow.
    if (bytes_value > task_frame_bins::max_block_bytes) {
        return bytes_value;
    }
    return task_frame_bins::class_bytes(bytes_value);
}

// True once this thread's cache has been destroyed. Coroutine frames can be
// freed during thread or static teardown after ~task_frame_cache has run; touching
// the cache then would use an object whose lifetime has ended. This flag is
// trivially destructible, so its storage outlives the cache's destructor and
// stays readable through teardown. The allocate/free paths fall back to the raw
// allocator once it is set.
thread_local bool task_frame_cache_destroyed = false;

// Frees this thread's cached frames at thread exit.
struct task_frame_cache final {
    ~task_frame_cache() {
        task_frame_cache_destroyed = true;
        release_all();
    }

    void release_all() noexcept {
        bins_.drain([](void* block, std::size_t) noexcept { free_mimalloc(block); });
    }

    task_frame_bins bins_;
};

thread_local task_frame_cache frame_cache;

}  // namespace

void* task_frame_allocate(std::size_t bytes_value) {
    const std::size_t class_bytes = task_frame_class_bytes(bytes_value == 0 ? 1 : bytes_value);
#if !RUVIA_SANITIZER_ALLOCATOR
    if (!task_frame_cache_destroyed && class_bytes <= task_frame_bins::max_block_bytes) {
        if (void* cached = frame_cache.bins_.take(class_bytes)) {
            return cached;
        }
    }
#endif
    return allocate_mimalloc(class_bytes, alignof(std::max_align_t));
}

void task_frame_deallocate_sized(void* pointer, std::size_t bytes_value) noexcept {
#if !RUVIA_SANITIZER_ALLOCATOR
    const std::size_t class_bytes = task_frame_class_bytes(bytes_value == 0 ? 1 : bytes_value);
    if (!task_frame_cache_destroyed && class_bytes <= task_frame_bins::max_block_bytes &&
        frame_cache.bins_.store(pointer, class_bytes, task_frame_cache_budget_bytes)) {
        return;
    }
#else
    (void)bytes_value;
#endif
    free_mimalloc(pointer);
}

void* task_frame_allocate_aligned(std::size_t bytes_value, std::size_t alignment) {
    return allocate_mimalloc(bytes_value, alignment);
}

void task_frame_deallocate_aligned(void* pointer) noexcept {
    free_mimalloc(pointer);
}

void task_frame_cache_release() noexcept {
    if (!task_frame_cache_destroyed) {
        frame_cache.release_all();
    }
}

}  // namespace detail

class worker_memory::impl_type final {
public:
    impl_type(std::pmr::memory_resource& upstream, std::size_t retained_block_bytes)
        : blocks_(upstream, retained_block_bytes) {}

    detail::worker_block_cache blocks_;
};

void worker_memory::impl_deleter_type::operator()(impl_type* impl) const noexcept {
    auto* const upstream = impl->blocks_.upstream_resource();
    destroy_pmr_object(impl, upstream);
}

void validate_memory_pool_config(const memory_pool_config& config) {
    if (config.request_initial_buffer_bytes_ == 0) {
        throw std::invalid_argument("memory pool request initial buffer size must be greater than zero");
    }
    if (config.retained_block_bytes_ > max_retained_block_bytes) {
        throw std::invalid_argument("memory pool retained block bytes exceed the addressable maximum");
    }
}

// The default upstream is the process-lifetime mimalloc adapter, not a shared
// pool. Each worker retains its own bounded caches and can also bind an
// explicit caller-owned upstream without changing its allocation contract.
worker_memory::worker_memory(const memory_pool_config& config)
    : worker_memory(*process_resource(), config) {}

worker_memory::worker_memory(std::pmr::memory_resource& upstream, const memory_pool_config& config)
    : impl_((validate_memory_pool_config(config),
          construct_pmr_object<impl_type>(&upstream, upstream, config.retained_block_bytes_))),
      resource_(&impl_->blocks_),
      request_initial_buffer_bytes_(config.request_initial_buffer_bytes_) {}

worker_memory::~worker_memory() = default;

void worker_memory::trim(const worker_handle& owner) {
    const auto owner_id = owner.id();
    if (owner_id == 0 || !owner.is_current() || (owner_id_ != 0 && owner_id != owner_id_)) {
        throw std::logic_error("worker memory trim must run on its owner worker");
    }
    owner_id_ = owner_id;
    impl_->blocks_.release_cached();
    detail::task_frame_cache_release();
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
