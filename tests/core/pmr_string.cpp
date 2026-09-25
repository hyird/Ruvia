#include <cstddef>
#include <memory_resource>
#include <string>

#include "ruvia/core/PmrString.h"

#include "test_harness.h"

namespace {

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t liveBytes{0};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        liveBytes += bytes;
        return result;
    }

    void do_deallocate(void* address, std::size_t bytes, std::size_t alignment) override {
        liveBytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(address, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace

RUVIA_TEST(pmr_string_clear_returns_large_storage_and_retains_small_storage) {
    CountingResource resource;
    {
        std::pmr::string buffer(&resource);
        const auto emptyBytes = resource.liveBytes;
        ruvia::resizePmrStringForOverwrite(buffer, 8192);
        RUVIA_CHECK_EQ(buffer.size(), std::size_t{8192});
        RUVIA_CHECK(resource.liveBytes > emptyBytes);
        ruvia::clearPmrStringRetainingSmall(buffer);
        RUVIA_CHECK(buffer.empty());
        RUVIA_CHECK_EQ(resource.liveBytes, emptyBytes);

        ruvia::resizePmrStringForOverwrite(buffer, 32);
        const auto retained = resource.liveBytes;
        RUVIA_CHECK(retained > emptyBytes);
        ruvia::clearPmrStringRetainingSmall(buffer);
        RUVIA_CHECK_EQ(resource.liveBytes, retained);
    }
    RUVIA_CHECK_EQ(resource.liveBytes, std::size_t{0});
}
