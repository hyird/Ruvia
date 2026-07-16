#include "ruvia/core/memory/MemoryPool.h"

#include <mimalloc.h>

// gcc signals TSan via __SANITIZE_THREAD__; clang via __has_feature. __has_feature
// must stay nested under its own defined() guard: gcc has no such builtin and
// would expand the bare token to 0, making `0(thread_sanitizer)` a preprocessor
// syntax error even though the && would short-circuit.
#if defined(__SANITIZE_THREAD__)
#define RUVIA_TSAN_ALLOCATOR_ANNOTATIONS 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define RUVIA_TSAN_ALLOCATOR_ANNOTATIONS 1
#endif
#endif

#if defined(RUVIA_TSAN_ALLOCATOR_ANNOTATIONS)
#include <sanitizer/tsan_interface.h>
#endif

namespace ruvia {

namespace detail {

void ensureMimallocGlobalOverrideLinked() noexcept;

namespace {

// mimalloc is not ThreadSanitizer-instrumented, so the happens-before edge it
// establishes when memory freed on one worker is handed to another (its internal
// free/alloc synchronization) is invisible to TSan. Under load that surfaces as
// a false "data race" between the previous owner's writes and the new owner's
// writes to the same reused address. Model the allocator's ordering explicitly:
// release the block on free, acquire it on allocate, so a later owner
// synchronizes-with the earlier one. Compiled out entirely without TSan.
inline void tsanAllocatorAcquire([[maybe_unused]] void* pointer) noexcept {
#if defined(RUVIA_TSAN_ALLOCATOR_ANNOTATIONS)
    if (pointer != nullptr) {
        __tsan_acquire(pointer);
    }
#endif
}

inline void tsanAllocatorRelease([[maybe_unused]] void* pointer) noexcept {
#if defined(RUVIA_TSAN_ALLOCATOR_ANNOTATIONS)
    if (pointer != nullptr) {
        __tsan_release(pointer);
    }
#endif
}

}  // namespace

}  // namespace detail

namespace {

struct DefaultResourceInstaller final {
    DefaultResourceInstaller() noexcept {
        detail::ensureMimallocGlobalOverrideLinked();
        (void)ProcessMemory::instance();
    }
};

DefaultResourceInstaller defaultResourceInstaller;

}  // namespace

namespace detail {

std::pmr::memory_resource* processResource() noexcept {
    return ProcessMemory::instance().upstreamResource();
}

void* taskFrameAllocate(std::size_t bytes) {
    void* pointer = mi_malloc(bytes == 0 ? 1 : bytes);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    tsanAllocatorAcquire(pointer);
    return pointer;
}

void taskFrameDeallocate(void* pointer) noexcept {
    tsanAllocatorRelease(pointer);
    mi_free(pointer);
}

}  // namespace detail

void* MimallocMemoryResource::do_allocate(std::size_t bytes, std::size_t alignment) {
    void* pointer = mi_malloc_aligned(bytes == 0 ? 1 : bytes, alignment);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }

    detail::tsanAllocatorAcquire(pointer);
    return pointer;
}

void MimallocMemoryResource::do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) {
    detail::tsanAllocatorRelease(pointer);
    mi_free_aligned(pointer, alignment);
    (void)bytes;
}

bool MimallocMemoryResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

ProcessMemory& ProcessMemory::instance() noexcept {
    static ProcessMemory processMemory;
    return processMemory;
}

ProcessMemory::ProcessMemory() {
    std::pmr::set_default_resource(&upstream_);
}

void ProcessMemory::configure(const MemoryPoolConfig& config) {
    if (frozen_) {
        throw std::logic_error("process memory configuration is already frozen");
    }
    config_ = config;
}

void ProcessMemory::freeze() noexcept {
    frozen_ = true;
}

MemoryPoolConfig ProcessMemory::config() const noexcept {
    return config_;
}

bool ProcessMemory::frozen() const noexcept {
    return frozen_;
}

std::pmr::memory_resource* ProcessMemory::upstreamResource() noexcept {
    return &upstream_;
}

MimallocMemoryResource& ProcessMemory::mimallocResource() noexcept {
    return upstream_;
}

WorkerMemory::WorkerMemory(const MemoryPoolConfig& config)
    : config_(config),
      resource_(detail::processResource()) {
    ProcessMemory::instance().freeze();
}

WorkerMemory::WorkerMemory(
    const MemoryPoolConfig& config,
    detail::DeferProcessMemoryFreeze)
    : config_(config),
      resource_(detail::processResource()) {}

std::pmr::memory_resource* WorkerMemory::resource() noexcept {
    return resource_;
}

std::pmr::memory_resource* WorkerMemory::resource() const noexcept {
    return resource_;
}

std::size_t WorkerMemory::requestInitialBufferBytes() const noexcept {
    return config_.requestInitialBufferBytes;
}

RequestMemory::RequestMemory(WorkerMemory& worker)
    : arena_(worker.requestInitialBufferBytes(), worker.resource()) {}

RequestMemory::RequestMemory(WorkerMemory& worker, std::span<std::byte> initialBuffer)
    : arena_(initialBuffer.data(), initialBuffer.size(), worker.resource()) {}

std::pmr::memory_resource* RequestMemory::resource() & noexcept {
    return &arena_;
}

std::pmr::memory_resource* RequestMemory::resource() const & noexcept {
    return const_cast<std::pmr::monotonic_buffer_resource*>(&arena_);
}

}  // namespace ruvia
