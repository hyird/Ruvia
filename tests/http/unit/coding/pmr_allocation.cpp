#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory_resource>
#include <stdexcept>

#include "ruvia/http/detail/coding/PmrCodecAllocation.h"
#include "ruvia/http/detail/coding/ZlibPmrAllocation.h"

#include "test_harness.h"

namespace {
class RecordingResource final : public std::pmr::memory_resource {
public:
    bool reject{false};
    std::size_t allocations{0};
    std::size_t releases{0};
    std::size_t allocatedBytes{0};
    std::size_t releasedBytes{0};
    std::size_t allocatedAlignment{0};
    std::size_t releasedAlignment{0};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        if (reject) {
            throw std::runtime_error("test allocation failure");
        }
        allocatedBytes = bytes;
        allocatedAlignment = alignment;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        ++releases;
        releasedBytes = bytes;
        releasedAlignment = alignment;
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace

RUVIA_TEST(codec_allocation_returns_aligned_blocks_to_original_resource) {
    RecordingResource original;
    RecordingResource other;
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{97}}) {
        auto* data = ruvia::detail::pmrCodecAllocate(&original, size);
        RUVIA_CHECK(data != nullptr);
        if (data == nullptr) {
            return;
        }
        RUVIA_CHECK(reinterpret_cast<std::uintptr_t>(data) % alignof(std::max_align_t) == 0);
        std::memset(data, 0x5a, size);
        ruvia::detail::pmrCodecFree(&other, data);
        RUVIA_CHECK(original.allocations == original.releases);
        RUVIA_CHECK(original.allocatedBytes == original.releasedBytes);
        RUVIA_CHECK(original.allocatedAlignment == original.releasedAlignment);
        RUVIA_CHECK(other.releases == 0);
    }
    ruvia::detail::pmrCodecFree(&original, nullptr);
    RUVIA_CHECK(original.allocations == original.releases);
}

RUVIA_TEST(zlib_allocation_adapts_element_counts_and_releases_storage) {
    RecordingResource resource;
    auto* data = ruvia::detail::zlibPmrAllocate(&resource, 17, 9);
    RUVIA_CHECK(data != nullptr);
    if (data == nullptr) {
        return;
    }
    RUVIA_CHECK(reinterpret_cast<std::uintptr_t>(data) % alignof(std::max_align_t) == 0);
    std::memset(data, 0x6b, 17 * 9);
    ruvia::detail::zlibPmrFree(data);
    ruvia::detail::zlibPmrFree(nullptr);
    RUVIA_CHECK(resource.allocations == resource.releases);
    RUVIA_CHECK(resource.allocatedBytes == resource.releasedBytes);
    RUVIA_CHECK(resource.allocatedAlignment == resource.releasedAlignment);
}

RUVIA_TEST(codec_allocation_rejects_invalid_sizes_and_contains_allocator_exceptions) {
    RecordingResource resource;
    RUVIA_CHECK(ruvia::detail::pmrCodecAllocate(nullptr, 1) == nullptr);
    RUVIA_CHECK(ruvia::detail::pmrCodecAllocate(
                    &resource, (std::numeric_limits<std::size_t>::max)()) == nullptr);
    RUVIA_CHECK(ruvia::detail::zlibPmrAllocate(nullptr, 1, 1) == nullptr);
    RUVIA_CHECK(ruvia::detail::zlibPmrAllocate(&resource, 0, 1) == nullptr);
    RUVIA_CHECK(ruvia::detail::zlibPmrAllocate(&resource, 1, 0) == nullptr);
    RUVIA_CHECK(resource.allocations == 0);
    resource.reject = true;
    RUVIA_CHECK(ruvia::detail::pmrCodecAllocate(&resource, 12) == nullptr);
    RUVIA_CHECK(ruvia::detail::zlibPmrAllocate(&resource, 12, 2) == nullptr);
    RUVIA_CHECK(resource.allocations == 2);
    RUVIA_CHECK(resource.releases == 0);
}
