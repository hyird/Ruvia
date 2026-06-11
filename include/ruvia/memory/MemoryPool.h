#pragma once

#include <cstddef>
#include <memory_resource>
#include <new>
#include <span>
#include <utility>

namespace ruvia {

struct MemoryPoolConfig {
    std::size_t requestInitialBufferBytes{4096};
};

struct MemoryStats {
    std::size_t totalAllocations{0};
    std::size_t totalDeallocations{0};
    std::size_t currentBytes{0};
    std::size_t peakBytes{0};
};

class MimallocMemoryResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] MemoryStats stats() const noexcept;
    void resetStats() noexcept;

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
        auto* storage = arena_.allocate(sizeof(T), alignof(T));
        auto* object = std::construct_at(static_cast<T*>(storage), std::forward<Args>(args)...);
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
