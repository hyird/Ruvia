#include "ruvia/core/memory/MemoryPool.h"

#include "ruvia/core/detail/TaskPromise.h"

#include <array>

namespace ruvia {

namespace {

struct DefaultResourceInstaller final {
    DefaultResourceInstaller() noexcept {
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
                ::operator delete(head);
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
            return cached;
        }
    }
    return ::operator new(classBytes);
}

void taskFrameDeallocate(void* pointer) noexcept {
    ::operator delete(pointer);
}

void taskFrameDeallocateSized(void* pointer, std::size_t bytes) noexcept {
    const std::size_t classBytes = taskFrameClassBytes(bytes == 0 ? 1 : bytes);
    if (!taskFrameCacheDestroyed && classBytes <= kTaskFrameCacheMaxBlockBytes
        && taskFrameCache.storeBlock(pointer, classBytes)) {
        return;
    }
    ::operator delete(pointer);
}

}  // namespace detail

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
    return &resource_;
}

std::pmr::memory_resource* WorkerMemory::resource() const noexcept {
    return const_cast<std::pmr::unsynchronized_pool_resource*>(&resource_);
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
