#pragma once

#include <array>
#include <cstddef>

namespace ruvia::detail {

// LIFO free lists of cached blocks keyed by size class. Class N holds blocks of
// exactly (N + 1) * granularity bytes, so any block in a bin serves every request
// that rounds to it; each block's first word links it to the next. The owner
// decides what is cacheable, allocates misses at the class ceiling and frees
// what store() refuses or drain() hands back. The cached total never exceeds the
// budget passed to store(). Unsynchronized: each owner is thread- or worker-affine.
template <std::size_t t_granularity, std::size_t t_max_block_bytes>
class size_class_lifo_bins final {
public:
    static_assert(t_granularity >= sizeof(void*) && (t_granularity & (t_granularity - 1)) == 0,
        "size class granularity must be a power of two that holds the link word");
    static_assert(t_max_block_bytes % t_granularity == 0, "the largest class must be a whole class");

    static constexpr std::size_t granularity = t_granularity;
    static constexpr std::size_t max_block_bytes = t_max_block_bytes;

    // The class ceiling of 1..max_block_bytes bytes.
    [[nodiscard]] static constexpr std::size_t class_bytes(std::size_t bytes_value) noexcept {
        return (bytes_value + granularity - 1) & ~(granularity - 1);
    }

    [[nodiscard]] void* take(std::size_t block_bytes) noexcept {
        void*& head = bins_[bin_index(block_bytes)];
        void* const block = head;
        if (block != nullptr) {
            head = *static_cast<void**>(block);
            cached_bytes_ -= block_bytes;
        }
        return block;
    }

    // Caches `block` of class `block_bytes` unless that would exceed
    // `budget_bytes`; false leaves the block with the caller.
    [[nodiscard]] bool store(void* block, std::size_t block_bytes, std::size_t budget_bytes) noexcept {
        // cached_bytes_ never exceeds the budget, so the subtraction cannot wrap.
        if (budget_bytes - cached_bytes_ < block_bytes) {
            return false;
        }
        void*& head = bins_[bin_index(block_bytes)];
        *static_cast<void**>(block) = head;
        head = block;
        cached_bytes_ += block_bytes;
        return true;
    }

    // Hands every cached block to release(block, block_bytes). Each block is
    // unlinked and accounted before the call, which may reenter these bins.
    template <typename release_type>
    void drain(release_type&& release) noexcept {
        for (std::size_t index = 0; index != bin_count; ++index) {
            const std::size_t block_bytes = (index + 1) * granularity;
            void*& head = bins_[index];
            while (head != nullptr) {
                void* const block = head;
                head = *static_cast<void**>(block);
                cached_bytes_ -= block_bytes;
                release(block, block_bytes);
            }
        }
    }

private:
    static constexpr std::size_t bin_count = max_block_bytes / granularity;

    [[nodiscard]] static constexpr std::size_t bin_index(std::size_t block_bytes) noexcept {
        return block_bytes / granularity - 1;
    }

    std::array<void*, bin_count> bins_{};
    std::size_t cached_bytes_{0};
};

}  // namespace ruvia::detail
