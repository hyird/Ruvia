#pragma once

#include <cstddef>
#include <memory_resource>

#include "size_class_lifo_bins.h"

namespace ruvia::detail {

// Worker-local exact-size cache. Small and large size classes share one
// retention budget; only freed blocks count against it. Overflow returns
// upstream immediately, and release_cached() never touches live allocations.
// Blocks above 1 MiB or with alignment above 64 pass straight through.
class worker_block_cache final : public std::pmr::memory_resource {
public:
    static constexpr std::size_t small_block_bytes = std::size_t{16} * 1024;
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
    using small_bins_type = size_class_lifo_bins<alignof(std::max_align_t), small_block_bytes>;
    using bins_type = size_class_lifo_bins<std::size_t{4} * 1024, std::size_t{1024} * 1024>;

    [[nodiscard]] static constexpr bool cacheable(std::size_t bytes_value, std::size_t alignment) noexcept {
        return bytes_value <= bins_type::max_block_bytes && alignment <= block_alignment;
    }

    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override;
    void do_deallocate(void* block, std::size_t bytes_value, std::size_t alignment) override;
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    std::pmr::memory_resource* upstream_;
    std::size_t retained_budget_bytes_;
    small_bins_type small_bins_;
    bins_type bins_;
};

}  // namespace ruvia::detail
