#pragma once

#include <array>
#include <cstddef>
#include <memory_resource>

namespace ruvia::detail {

// Upstream of a worker's small-object pool: an exact-size LIFO cache for
// freed blocks above the pool cutoff. Sizes are rounded to 4 KiB, so every
// request mapping to a bin can be served by any block stored in it. Freed
// blocks are kept while the cached total stays within the retention budget;
// the excess goes back upstream at once, and release_cached() returns every
// cached block. Only freed blocks are ever held; live allocations are never
// touched. Blocks above 1 MiB or with alignment above 64 pass straight through.
// Worker-affine with no synchronization, like the pool it serves.
class worker_block_cache final : public std::pmr::memory_resource {
public:
    static constexpr std::size_t small_block_bytes = std::size_t{16} * 1024;
    static constexpr std::size_t granularity_bytes = std::size_t{4} * 1024;
    static constexpr std::size_t max_cached_block_bytes = std::size_t{1024} * 1024;
    static constexpr std::size_t block_alignment = 64;

    // upstream must outlive this cache.
    worker_block_cache(std::pmr::memory_resource& upstream, std::size_t retained_budget_bytes) noexcept;
    ~worker_block_cache() override;

    worker_block_cache(const worker_block_cache&) = delete;
    worker_block_cache& operator=(const worker_block_cache&) = delete;

    void release_cached() noexcept;

    [[nodiscard]] std::pmr::memory_resource* upstream_resource() const noexcept {
        return upstream_;
    }

private:
    static constexpr std::size_t bin_count = max_cached_block_bytes / granularity_bytes;

    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override;
    void do_deallocate(void* block, std::size_t bytes_value, std::size_t alignment) override;
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    std::pmr::memory_resource* upstream_;
    std::size_t retained_budget_bytes_;
    std::size_t cached_bytes_{0};
    std::array<void*, bin_count> bins_{};
};

}  // namespace ruvia::detail
