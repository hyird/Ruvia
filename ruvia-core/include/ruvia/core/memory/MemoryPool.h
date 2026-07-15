#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <utility>

namespace ruvia {

namespace detail {
struct DeferProcessMemoryFreeze final {};
}

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

class MimallocMemoryResource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override;
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;
};

class ProcessMemory final {
public:
    [[nodiscard]] static ProcessMemory& instance() noexcept;

    void configure(const MemoryPoolConfig& config);
    void freeze() noexcept;

    [[nodiscard]] MemoryPoolConfig config() const noexcept;
    [[nodiscard]] bool frozen() const noexcept;
    [[nodiscard]] std::pmr::memory_resource* upstreamResource() noexcept;
    [[nodiscard]] MimallocMemoryResource& mimallocResource() noexcept;

    ProcessMemory(const ProcessMemory&) = delete;
    ProcessMemory& operator=(const ProcessMemory&) = delete;

private:
    ProcessMemory();

    MemoryPoolConfig config_;
    MimallocMemoryResource upstream_;
    bool frozen_{false};
};

class WorkerMemory final {
public:
    explicit WorkerMemory(const MemoryPoolConfig& config = ProcessMemory::instance().config());
    WorkerMemory(
        const MemoryPoolConfig& config,
        detail::DeferProcessMemoryFreeze);

    WorkerMemory(const WorkerMemory&) = delete;
    WorkerMemory& operator=(const WorkerMemory&) = delete;

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() noexcept {
        return std::pmr::polymorphic_allocator<T>(resource_);
    }

    [[nodiscard]] std::pmr::memory_resource* resource() noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] std::size_t requestInitialBufferBytes() const noexcept;

private:
    MemoryPoolConfig config_;
    std::pmr::memory_resource* resource_;
};

class RequestMemory final {
public:
    explicit RequestMemory(WorkerMemory& worker);
    RequestMemory(WorkerMemory& worker, std::span<std::byte> initialBuffer);
    ~RequestMemory();

    RequestMemory(const RequestMemory&) = delete;
    RequestMemory& operator=(const RequestMemory&) = delete;

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() noexcept {
        return std::pmr::polymorphic_allocator<T>(&arena_);
    }

    [[nodiscard]] std::pmr::memory_resource* resource() noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;

    template <typename T, typename... Args>
    T& emplace(Args&&... args) {
        auto* node = static_cast<CleanupNode*>(arena_.allocate(sizeof(CleanupNode), alignof(CleanupNode)));
        // Construct directly in the request arena so the cleanup node and the
        // object share one lifetime domain without an extra owning wrapper.
        auto* storage = arena_.allocate(sizeof(T), alignof(T));
        T* object;
        try {
            object = std::construct_at(static_cast<T*>(storage), std::forward<Args>(args)...);
        } catch (...) {
            arena_.deallocate(storage, sizeof(T), alignof(T));
            throw;
        }
        node->object = object;
        node->destroy = [](void* value) noexcept {
            std::destroy_at(static_cast<T*>(value));
        };
        node->next = cleanupHead_;
        cleanupHead_ = node;
        return *object;
    }

private:
    struct CleanupNode {
        CleanupNode* next{nullptr};
        void* object{nullptr};
        void (*destroy)(void*) noexcept{nullptr};
    };

    std::pmr::monotonic_buffer_resource arena_;
    CleanupNode* cleanupHead_{nullptr};
};

}  // namespace ruvia
