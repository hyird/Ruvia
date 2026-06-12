#include "ruvia/memory/MemoryPool.h"

#include <mimalloc.h>

namespace ruvia {

namespace detail {

void ensureMimallocGlobalOverrideLinked() noexcept;

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

void* taskFrameAllocate(std::size_t bytes) {
    void* pointer = mi_malloc(bytes == 0 ? 1 : bytes);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void taskFrameDeallocate(void* pointer) noexcept {
    mi_free(pointer);
}

}  // namespace detail

MemoryStats MimallocMemoryResource::stats() const noexcept {
    return {};
}

void MimallocMemoryResource::resetStats() noexcept {
}

void* MimallocMemoryResource::do_allocate(std::size_t bytes, std::size_t alignment) {
    void* pointer = mi_malloc_aligned(bytes == 0 ? 1 : bytes, alignment);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }

    return pointer;
}

void MimallocMemoryResource::do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) {
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
      resource_(ProcessMemory::instance().upstreamResource()) {
    ProcessMemory::instance().freeze();
}

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

RequestMemory::~RequestMemory() {
    while (cleanupHead_ != nullptr) {
        auto* node = cleanupHead_;
        cleanupHead_ = cleanupHead_->next;
        if (node->destroy != nullptr) {
            node->destroy(node->object);
        }
    }
}

std::pmr::memory_resource* RequestMemory::resource() noexcept {
    return &arena_;
}

std::pmr::memory_resource* RequestMemory::resource() const noexcept {
    return const_cast<std::pmr::monotonic_buffer_resource*>(&arena_);
}

}  // namespace ruvia
