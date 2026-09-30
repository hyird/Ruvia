#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>

#include "ruvia/core/memory/MemoryPoolConfig.h"

namespace ruvia {

// Explicit worker-local owner, not thread_local storage. All allocations and
// deallocations are worker-affine; this pool has no internal synchronization.
// Individual deallocations become reusable pool storage. The pool can retain
// cached blocks until destruction and must outlive every object using it.
class WorkerMemory final {
public:
    explicit WorkerMemory(const MemoryPoolConfig& config = {});
    // Bind an upstream once at construction (for custom allocation/accounting).
    // It must outlive this pool and all of its borrowers. The default overload
    // continues to use Ruvia's process resource, never the global PMR default.
    explicit WorkerMemory(std::pmr::memory_resource& upstream,
        const MemoryPoolConfig& config = {});
    ~WorkerMemory();

    WorkerMemory(const WorkerMemory&) = delete;
    WorkerMemory& operator=(const WorkerMemory&) = delete;

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() & noexcept {
        return std::pmr::polymorphic_allocator<T>(resource_);
    }

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() && = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() & noexcept {
        return resource_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const& noexcept {
        return resource_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() && = delete;
    [[nodiscard]] std::pmr::memory_resource* resource() const&& = delete;
    [[nodiscard]] std::size_t requestInitialBufferBytes() const noexcept {
        return requestInitialBufferBytes_;
    }

private:
    class Impl;
    struct ImplDeleter {
        void operator()(Impl* impl) const noexcept;
    };
    std::unique_ptr<Impl, ImplDeleter> impl_;
    // Cache the resource borrow and configuration without a hot-path Impl lookup.
    std::pmr::memory_resource* resource_;
    std::size_t requestInitialBufferBytes_;
};

// Request/handshake lifetime arena borrowing its worker and optional initial
// buffer. Both must outlive this object. Individual deallocation is a no-op;
// destruction returns overflow blocks to the worker, not necessarily the OS.
// Objects using the arena must be destroyed before the arena itself.
class RequestMemory final {
public:
    explicit RequestMemory(WorkerMemory& worker);
    RequestMemory(WorkerMemory& worker, std::span<std::byte> initialBuffer);
    ~RequestMemory() = default;

    RequestMemory(const RequestMemory&) = delete;
    RequestMemory& operator=(const RequestMemory&) = delete;

    // Independent arena with the same worker-owned upstream. The child may
    // outlive this arena, but never the worker that owns its storage.
    [[nodiscard]] RequestMemory fork() const&;
    RequestMemory fork() const&& = delete;

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() & noexcept {
        return std::pmr::polymorphic_allocator<T>(&arena_);
    }

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() && = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() & noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const& noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() && = delete;
    [[nodiscard]] std::pmr::memory_resource* resource() const&& = delete;

    // Reclaimable storage owned by the same worker, independent of this arena.
    [[nodiscard]] std::pmr::memory_resource* upstreamResource() & noexcept;
    [[nodiscard]] std::pmr::memory_resource* upstreamResource() const& noexcept;
    [[nodiscard]] std::pmr::memory_resource* upstreamResource() && = delete;
    [[nodiscard]] std::pmr::memory_resource* upstreamResource() const&& = delete;

private:
    struct ChildArena {};
    RequestMemory(ChildArena, std::pmr::memory_resource* upstream);
    mutable std::pmr::monotonic_buffer_resource arena_;
};

}  // namespace ruvia
