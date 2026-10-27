#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>

#include "ruvia/core/worker_handle.h"

namespace ruvia {

// Default initial bump-block size for a request arena: a request whose
// allocations fit within it touches no further heap. Runtime integrations that
// keep a reusable block per connection size it from the configured
// request_initial_buffer_bytes_, so a larger value stays free of per-request
// heap allocation at the cost of that much storage per active connection.
inline constexpr std::size_t request_arena_initial_bytes = std::size_t{4} * 1024;

// Default upper bound on the large blocks one worker_memory keeps cached for
// reuse after they are freed.
inline constexpr std::size_t default_retained_block_bytes = std::size_t{8} * 1024 * 1024;
// Largest accepted retained_block_bytes_: no address space can hold more.
inline constexpr std::size_t max_retained_block_bytes = static_cast<std::size_t>(PTRDIFF_MAX);

// request_initial_buffer_bytes_ must be greater than zero: it is the initial
// block size of every request arena.
// retained_block_bytes_ bounds the freed large blocks (above 16 KiB, up to
// 1 MiB, sizes rounded to 4 KiB) a worker keeps for exact-size reuse; blocks
// freed beyond it are returned immediately. Zero disables the cache; values
// above max_retained_block_bytes are rejected.
struct memory_pool_config {
    std::size_t request_initial_buffer_bytes_{request_arena_initial_bytes};
    std::size_t retained_block_bytes_{default_retained_block_bytes};
};

// The single validation used by worker_memory construction and by integrations
// that check startup configuration early. Throws std::invalid_argument.
void validate_memory_pool_config(const memory_pool_config& config);

// Explicit worker-local owner, not thread_local storage. All allocations and
// deallocations are worker-affine; this pool has no internal synchronization.
//
// Two tiers: objects up to 16 KiB come from a small-object pool whose freed
// storage stays reusable for the worker's life. Larger blocks come from a
// bounded block cache: freed blocks up to 1 MiB (alignment up to 64) are kept,
// in 4 KiB-rounded exact-size bins, for reuse while the cached total stays
// within memory_pool_config::retained_block_bytes_; the excess goes back
// upstream at once, and trim() returns every cached block. Larger or
// over-aligned blocks pass straight through. Whether returned memory reaches
// the OS is up to the C allocator. The pool must outlive every object using it.
class worker_memory final {
public:
    // Uses the process-lifetime mimalloc resource directly, never the global
    // PMR default. The upstream has no shared pooling layer.
    explicit worker_memory(const memory_pool_config& config = {});
    // Bind an upstream once at construction (for custom allocation/accounting).
    // It must outlive this pool and all of its borrowers, and receives every
    // block, including the pool object's own storage.
    explicit worker_memory(std::pmr::memory_resource& upstream,
        const memory_pool_config& config = {});
    ~worker_memory();

    worker_memory(const worker_memory&) = delete;
    worker_memory& operator=(const worker_memory&) = delete;

    // Returns cached large blocks upstream and drains the calling thread's
    // coroutine frame cache; live allocations and the small-object pool are
    // untouched. Blocks newly cached by a reentrant upstream call may remain.
    // Owner thread only: the first call binds `owner` as the worker this memory
    // serves, and every call throws std::logic_error unless it runs on that worker.
    void trim(const worker_handle& owner);

    template <typename t_type = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<t_type> allocator() & noexcept {
        return std::pmr::polymorphic_allocator<t_type>(resource_);
    }

    template <typename t_type = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<t_type> allocator() && = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() & noexcept {
        return resource_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const& noexcept {
        return resource_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() && = delete;
    [[nodiscard]] std::pmr::memory_resource* resource() const&& = delete;
    [[nodiscard]] std::size_t request_initial_buffer_bytes() const noexcept {
        return request_initial_buffer_bytes_;
    }

private:
    class impl_type;
    struct impl_deleter_type {
        void operator()(impl_type* impl) const noexcept;
    };
    std::unique_ptr<impl_type, impl_deleter_type> impl_;
    // Cache the resource borrow and configuration without a hot-path Impl lookup.
    std::pmr::memory_resource* resource_;
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
    [[nodiscard]] std::pmr::polymorphic_allocator<t_type> allocator() & noexcept {
        return std::pmr::polymorphic_allocator<t_type>(&arena_);
    }

    template <typename t_type = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<t_type> allocator() && = delete;

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
