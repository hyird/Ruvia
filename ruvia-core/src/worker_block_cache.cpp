#include "worker_block_cache.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] constexpr bool cacheable(std::size_t bytes_value, std::size_t alignment) noexcept {
    return bytes_value > worker_block_cache::small_block_bytes &&
           bytes_value <= worker_block_cache::max_cached_block_bytes &&
           alignment <= worker_block_cache::block_alignment;
}

[[nodiscard]] constexpr std::size_t round_to_granularity(std::size_t bytes_value) noexcept {
    return (bytes_value + worker_block_cache::granularity_bytes - 1) &
           ~(worker_block_cache::granularity_bytes - 1);
}

[[nodiscard]] constexpr std::size_t bin_index(std::size_t block_bytes) noexcept {
    return block_bytes / worker_block_cache::granularity_bytes - 1;
}

}  // namespace

worker_block_cache::worker_block_cache(
    std::pmr::memory_resource& upstream, std::size_t retained_budget_bytes) noexcept
    : upstream_(&upstream),
      retained_budget_bytes_(retained_budget_bytes) {}

worker_block_cache::~worker_block_cache() {
    release_cached();
}

void worker_block_cache::release_cached() noexcept {
    for (std::size_t index = 0; index < bin_count; ++index) {
        const std::size_t block_bytes = (index + 1) * granularity_bytes;
        void*& head = bins_[index];
        while (head != nullptr) {
            // The upstream may reenter this cache while releasing the block.
            // Publish its removal and accounting before calling user code.
            void* const block = head;
            head = *static_cast<void**>(block);
            cached_bytes_ -= block_bytes;
            upstream_->deallocate(block, block_bytes, block_alignment);
        }
    }
}

void* worker_block_cache::do_allocate(std::size_t bytes_value, std::size_t alignment) {
    if (!cacheable(bytes_value, alignment)) {
        return upstream_->allocate(bytes_value, alignment);
    }
    const std::size_t block_bytes = round_to_granularity(bytes_value);
    void*& head = bins_[bin_index(block_bytes)];
    if (void* const block = head; block != nullptr) {
        head = *static_cast<void**>(block);
        cached_bytes_ -= block_bytes;
        return block;
    }
    return upstream_->allocate(block_bytes, block_alignment);
}

void worker_block_cache::do_deallocate(void* block, std::size_t bytes_value, std::size_t alignment) {
    if (!cacheable(bytes_value, alignment)) {
        upstream_->deallocate(block, bytes_value, alignment);
        return;
    }
    const std::size_t block_bytes = round_to_granularity(bytes_value);
    // cached_bytes_ never exceeds the budget, so the subtraction cannot wrap.
    if (block_bytes > retained_budget_bytes_ - cached_bytes_) {
        upstream_->deallocate(block, block_bytes, block_alignment);
        return;
    }
    void*& head = bins_[bin_index(block_bytes)];
    *static_cast<void**>(block) = head;
    head = block;
    cached_bytes_ += block_bytes;
}

bool worker_block_cache::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

}  // namespace ruvia::detail
