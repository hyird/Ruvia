#include "ruvia/core/memory/MemoryPool.h"

#include "ruvia/core/detail/TaskPromise.h"

#include <array>

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

namespace {

// Coroutine frames come and go several times per request, always in a handful
// of recurring sizes. A thread-local LIFO cache keyed by 128-byte size class
// hands a just-freed frame straight to the next allocation of the same class,
// so the request hot path skips the general allocator and reuses cache-warm
// memory. Blocks are allocated at the class ceiling, which keeps every cached
// block large enough for any request that maps to its bin. Only the sized
// deallocation path may cache: the unsized fallback cannot know the class.
constexpr std::size_t kTaskFrameCacheGranularity = 128;
constexpr std::size_t kTaskFrameCacheMaxBlockBytes = 8 * 1024;
constexpr std::size_t kTaskFrameCacheBudgetBytes = 128 * 1024;
constexpr std::size_t kTaskFrameCacheBinCount =
    kTaskFrameCacheMaxBlockBytes / kTaskFrameCacheGranularity;

[[nodiscard]] constexpr std::size_t taskFrameClassBytes(std::size_t bytes) noexcept {
    return (bytes + kTaskFrameCacheGranularity - 1) & ~(kTaskFrameCacheGranularity - 1);
}

// True once this thread's cache has been destroyed. Coroutine frames can be
// freed during thread or static teardown after ~TaskFrameCache has run; touching
// the cache then would use an object whose lifetime has ended. This flag is
// trivially destructible, so its storage outlives the cache's destructor and
// stays readable through teardown. The allocate/free paths fall back to the raw
// allocator once it is set.
thread_local bool taskFrameCacheDestroyed = false;

class TaskFrameCache final {
public:
    TaskFrameCache() noexcept = default;
    TaskFrameCache(const TaskFrameCache&) = delete;
    TaskFrameCache& operator=(const TaskFrameCache&) = delete;

    ~TaskFrameCache() {
        taskFrameCacheDestroyed = true;
        for (void*& head : bins_) {
            while (head != nullptr) {
                void* next = *static_cast<void**>(head);
                mi_free(head);
                head = next;
            }
        }
    }

    [[nodiscard]] void* takeBlock(std::size_t classBytes) noexcept {
        void*& head = bins_[binIndex(classBytes)];
        void* block = head;
        if (block != nullptr) {
            head = *static_cast<void**>(block);
            cachedBytes_ -= classBytes;
        }
        return block;
    }

    [[nodiscard]] bool storeBlock(void* block, std::size_t classBytes) noexcept {
        if (cachedBytes_ + classBytes > kTaskFrameCacheBudgetBytes) {
            return false;
        }
        void*& head = bins_[binIndex(classBytes)];
        *static_cast<void**>(block) = head;
        head = block;
        cachedBytes_ += classBytes;
        return true;
    }

private:
    [[nodiscard]] static constexpr std::size_t binIndex(std::size_t classBytes) noexcept {
        return classBytes / kTaskFrameCacheGranularity - 1;
    }

    std::array<void*, kTaskFrameCacheBinCount> bins_{};
    std::size_t cachedBytes_{0};
};

thread_local TaskFrameCache taskFrameCache;

}  // namespace

void* taskFrameAllocate(std::size_t bytes) {
    const std::size_t classBytes = taskFrameClassBytes(bytes == 0 ? 1 : bytes);
    if (!taskFrameCacheDestroyed && classBytes <= kTaskFrameCacheMaxBlockBytes) {
        if (void* cached = taskFrameCache.takeBlock(classBytes)) {
            tsanAllocatorAcquire(cached);
            return cached;
        }
    }
    void* pointer = mi_malloc(classBytes);
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

void taskFrameDeallocateSized(void* pointer, std::size_t bytes) noexcept {
    tsanAllocatorRelease(pointer);
    const std::size_t classBytes = taskFrameClassBytes(bytes == 0 ? 1 : bytes);
    if (!taskFrameCacheDestroyed && classBytes <= kTaskFrameCacheMaxBlockBytes
        && taskFrameCache.storeBlock(pointer, classBytes)) {
        return;
    }
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
