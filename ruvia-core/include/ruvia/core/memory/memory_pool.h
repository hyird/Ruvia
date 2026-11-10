#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>

#include "ruvia/core/worker_handle.h"

struct mi_heap_s;

namespace ruvia {

// Default initial bump-block size for a request arena: a request whose
// allocations fit within it touches no further heap. Runtime integrations that
// keep a reusable block per connection size it from the configured
// request_initial_buffer_bytes_, so a larger value stays free of per-request
// heap allocation at the cost of that much storage per active connection.
inline constexpr std::size_t request_arena_initial_bytes = std::size_t{4} * 1024;

// request_initial_buffer_bytes_ must be greater than zero: it is the initial
// block size of every request arena.
struct memory_pool_config final {
    std::size_t request_initial_buffer_bytes_{request_arena_initial_bytes};
};

// The single validation used by worker_memory construction and by integrations
// that check startup configuration early. Throws std::invalid_argument.
void validate_memory_pool_config(const memory_pool_config& config);

// Worker-owned mimalloc heap; construction and destruction may run on a
// different thread from the worker. The resource must outlive all borrowers,
// and destruction requires that no thread is still using the heap.
class worker_memory final : private std::pmr::memory_resource {
public:
    // Owns a native heap without replacing either allocator's default resource.
    explicit worker_memory(const memory_pool_config& config = {});
    ~worker_memory() override;

    worker_memory(const worker_memory&) = delete;
    worker_memory& operator=(const worker_memory&) = delete;

    // Collects the calling thread's default heap and this worker heap through
    // mimalloc, with one final forced arena purge. Live allocations are untouched.
    // Owner thread only: the first call binds `owner` as the worker this memory
    // serves, and every call throws std::logic_error unless it runs on that worker.
    void trim(const worker_handle& owner);

    template <typename t_type = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<t_type> allocator() const& noexcept {
        return std::pmr::polymorphic_allocator<t_type>(resource());
    }

    template <typename t_type = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<t_type> allocator() const&& = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() & noexcept {
        return this;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const& noexcept {
        // Allocation mutates the owned heap, not the resource object.
        return const_cast<worker_memory*>(this);
    }
    [[nodiscard]] std::pmr::memory_resource* resource() && = delete;
    [[nodiscard]] std::pmr::memory_resource* resource() const&& = delete;
    [[nodiscard]] std::size_t request_initial_buffer_bytes() const noexcept {
        return request_initial_buffer_bytes_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override;
    void do_deallocate(void* block, std::size_t, std::size_t) noexcept override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    mi_heap_s* heap_{nullptr};
    std::size_t request_initial_buffer_bytes_;
    // Bound by the first trim(); 0 until then.
    worker_id_type owner_id_{0};
};

// Request/handshake lifetime arena borrowing its worker and optional initial
// buffer. Both must outlive this object. Individual deallocation is a no-op;
// destruction returns overflow blocks to the worker, not necessarily the OS.
// Objects using the arena must be destroyed before the arena itself.
class request_memory final {
public:
    explicit request_memory(worker_memory& worker_value);
    request_memory(worker_memory& worker_value, std::span<std::byte> initial_buffer);
    ~request_memory() = default;

    request_memory(const request_memory&) = delete;
    request_memory& operator=(const request_memory&) = delete;

    // Independent arena with the same worker-owned upstream and the same
    // initial block size. The child may outlive this arena, but never the
    // worker that owns its storage.
    [[nodiscard]] request_memory fork() const&;
    request_memory fork() const&& = delete;

    template <typename t_type = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<t_type> allocator() const& noexcept {
        return std::pmr::polymorphic_allocator<t_type>(&arena_);
    }

    template <typename t_type = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<t_type> allocator() const&& = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() & noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const& noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() && = delete;
    [[nodiscard]] std::pmr::memory_resource* resource() const&& = delete;

    // Reclaimable storage owned by the same worker, independent of this arena.
    [[nodiscard]] std::pmr::memory_resource* upstream_resource() & noexcept;
    [[nodiscard]] std::pmr::memory_resource* upstream_resource() const& noexcept;
    [[nodiscard]] std::pmr::memory_resource* upstream_resource() && = delete;
    [[nodiscard]] std::pmr::memory_resource* upstream_resource() const&& = delete;

private:
    request_memory(std::size_t initial_bytes, std::pmr::memory_resource* upstream);
    mutable std::pmr::monotonic_buffer_resource arena_;
    std::size_t initial_bytes_;
};

}  // namespace ruvia
