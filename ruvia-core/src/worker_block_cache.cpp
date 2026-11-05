#include "worker_block_cache.h"

namespace ruvia::detail {

worker_block_cache::worker_block_cache(
    std::pmr::memory_resource& upstream, std::size_t retained_budget_bytes) noexcept
    : upstream_(&upstream),
      retained_budget_bytes_(retained_budget_bytes) {}

worker_block_cache::~worker_block_cache() {
    release_cached();
}

void worker_block_cache::release_cached() noexcept {
    // The upstream may reenter this cache while releasing a block; drain
    // publishes each removal before the call.
    bins_.drain([this](void* block, std::size_t block_bytes) noexcept {
        upstream_->deallocate(block, block_bytes, block_alignment);
    });
}

void* worker_block_cache::do_allocate(std::size_t bytes_value, std::size_t alignment) {
    if (!cacheable(bytes_value, alignment)) {
        return upstream_->allocate(bytes_value, alignment);
    }
    const std::size_t block_bytes = bins_type::class_bytes(bytes_value);
    if (void* const block = bins_.take(block_bytes)) {
        return block;
    }
    return upstream_->allocate(block_bytes, block_alignment);
}

void worker_block_cache::do_deallocate(void* block, std::size_t bytes_value, std::size_t alignment) {
    if (!cacheable(bytes_value, alignment)) {
        upstream_->deallocate(block, bytes_value, alignment);
        return;
    }
    const std::size_t block_bytes = bins_type::class_bytes(bytes_value);
    if (!bins_.store(block, block_bytes, retained_budget_bytes_)) {
        upstream_->deallocate(block, block_bytes, block_alignment);
    }
}

bool worker_block_cache::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

}  // namespace ruvia::detail
