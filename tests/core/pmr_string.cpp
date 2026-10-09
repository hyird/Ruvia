#include "ruvia/core/pmr_string.h"

#include <cstddef>
#include <memory_resource>
#include <string>

#include "test_harness.h"

namespace {

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t live_bytes_{0};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        void* result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        live_bytes_ += bytes_value;
        return result_value;
    }

    void do_deallocate(void* address, std::size_t bytes_value, std::size_t alignment) override {
        live_bytes_ -= bytes_value;
        std::pmr::new_delete_resource()->deallocate(address, bytes_value, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace

RUVIA_TEST(pmr_string_clear_returns_large_storage_and_retains_small_storage) {
    counting_resource resource;
    {
        std::pmr::string buffer(&resource);
        const auto empty_bytes = resource.live_bytes_;
        ruvia::resize_pmr_string_for_overwrite(buffer, 8192);
        RUVIA_CHECK_EQ(buffer.size(), std::size_t{8192});
        RUVIA_CHECK(resource.live_bytes_ > empty_bytes);
        ruvia::clear_pmr_string_retaining_small(buffer);
        RUVIA_CHECK(buffer.empty());
        RUVIA_CHECK_EQ(resource.live_bytes_, empty_bytes);

        ruvia::resize_pmr_string_for_overwrite(buffer, 32);
        const auto retained = resource.live_bytes_;
        RUVIA_CHECK(retained > empty_bytes);
        ruvia::clear_pmr_string_retaining_small(buffer);
        RUVIA_CHECK_EQ(resource.live_bytes_, retained);
    }
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
}
