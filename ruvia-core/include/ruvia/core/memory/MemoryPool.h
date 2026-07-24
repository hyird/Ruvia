#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>

namespace ruvia {

// Default initial bump-block size for a request arena. Runtime integrations size
// their connection-private dispatch blocks to this same constant, so configured
// defaults and compile-time blocks stay in lockstep: a request whose allocations
// fit within it touches no heap at all.
//
// Configuring requestInitialBufferBytes larger than this constant stays correct
// but spills the arena's first block to the worker resource on every request,
// because the in-block fast path is sized at compile time. Prefer raising
// kRequestArenaInitialBytes itself if a larger zero-heap default is wanted.
inline constexpr std::size_t kRequestArenaInitialBytes = 4 * 1024;

struct MemoryPoolConfig {
    std::size_t requestInitialBufferBytes{kRequestArenaInitialBytes};
};

class WorkerMemory final {
public:
    explicit WorkerMemory(const MemoryPoolConfig& config = {});

    WorkerMemory(const WorkerMemory&) = delete;
    WorkerMemory& operator=(const WorkerMemory&) = delete;

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() noexcept {
        return std::pmr::polymorphic_allocator<T>(&resource_);
    }

    [[nodiscard]] std::pmr::memory_resource* resource() noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] std::size_t requestInitialBufferBytes() const noexcept;

private:
    MemoryPoolConfig config_;
    std::pmr::unsynchronized_pool_resource resource_;
};

class RequestMemory final {
public:
    explicit RequestMemory(WorkerMemory& worker);
    RequestMemory(WorkerMemory& worker, std::span<std::byte> initialBuffer);
    ~RequestMemory() = default;

    RequestMemory(const RequestMemory&) = delete;
    RequestMemory& operator=(const RequestMemory&) = delete;

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

private:
    std::pmr::monotonic_buffer_resource arena_;
};

}  // namespace ruvia
