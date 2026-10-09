#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory_resource>
#include <stdexcept>

#include "coding/pmr_codec_allocation.h"
#include "coding/zlib_pmr_allocation.h"
#include "test_harness.h"

namespace {
class recording_resource final : public std::pmr::memory_resource {
public:
    bool reject_{false};
    std::size_t allocations_{0};
    std::size_t releases_{0};
    std::size_t allocated_bytes_{0};
    std::size_t released_bytes_{0};
    std::size_t allocated_alignment_{0};
    std::size_t released_alignment_{0};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocations_;
        if (reject_) {
            throw std::runtime_error("test allocation failure");
        }
        allocated_bytes_ = bytes_value;
        allocated_alignment_ = alignment;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* value, std::size_t bytes_value, std::size_t alignment) override {
        ++releases_;
        released_bytes_ = bytes_value;
        released_alignment_ = alignment;
        std::pmr::new_delete_resource()->deallocate(value, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace

RUVIA_TEST(codec_allocation_returns_aligned_blocks_to_original_resource) {
    recording_resource original;
    recording_resource other;
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{97}}) {
        auto* data = ruvia::detail::pmr_codec_allocate(&original, size);
        RUVIA_CHECK(data != nullptr);
        if (data == nullptr) {
            return;
        }
        RUVIA_CHECK(reinterpret_cast<std::uintptr_t>(data) % alignof(std::max_align_t) == 0);
        std::memset(data, 0x5a, size);
        ruvia::detail::pmr_codec_free(&other, data);
        RUVIA_CHECK(original.allocations_ == original.releases_);
        RUVIA_CHECK(original.allocated_bytes_ == original.released_bytes_);
        RUVIA_CHECK(original.allocated_alignment_ == original.released_alignment_);
        RUVIA_CHECK(other.releases_ == 0);
    }
    ruvia::detail::pmr_codec_free(&original, nullptr);
    RUVIA_CHECK(original.allocations_ == original.releases_);
}

RUVIA_TEST(zlib_allocation_adapts_element_counts_and_releases_storage) {
    recording_resource resource;
    auto* data = ruvia::detail::zlib_pmr_allocate(&resource, 17, 9);
    RUVIA_CHECK(data != nullptr);
    if (data == nullptr) {
        return;
    }
    RUVIA_CHECK(reinterpret_cast<std::uintptr_t>(data) % alignof(std::max_align_t) == 0);
    std::memset(data, 0x6b, 17 * 9);
    ruvia::detail::zlib_pmr_free(data);
    ruvia::detail::zlib_pmr_free(nullptr);
    RUVIA_CHECK(resource.allocations_ == resource.releases_);
    RUVIA_CHECK(resource.allocated_bytes_ == resource.released_bytes_);
    RUVIA_CHECK(resource.allocated_alignment_ == resource.released_alignment_);
}

RUVIA_TEST(codec_allocation_rejects_invalid_sizes_and_contains_allocator_exceptions) {
    recording_resource resource;
    RUVIA_CHECK(ruvia::detail::pmr_codec_allocate(nullptr, 1) == nullptr);
    RUVIA_CHECK(ruvia::detail::pmr_codec_allocate(
                    &resource, (std::numeric_limits<std::size_t>::max)()) == nullptr);
    RUVIA_CHECK(ruvia::detail::zlib_pmr_allocate(nullptr, 1, 1) == nullptr);
    RUVIA_CHECK(ruvia::detail::zlib_pmr_allocate(&resource, 0, 1) == nullptr);
    RUVIA_CHECK(ruvia::detail::zlib_pmr_allocate(&resource, 1, 0) == nullptr);
    RUVIA_CHECK(resource.allocations_ == 0);
    resource.reject_ = true;
    RUVIA_CHECK(ruvia::detail::pmr_codec_allocate(&resource, 12) == nullptr);
    RUVIA_CHECK(ruvia::detail::zlib_pmr_allocate(&resource, 12, 2) == nullptr);
    RUVIA_CHECK(resource.allocations_ == 2);
    RUVIA_CHECK(resource.releases_ == 0);
}
