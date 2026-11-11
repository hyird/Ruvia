#include <gtest/gtest.h>

#include <cstddef>
#include <memory_resource>
#include <string>

#include "ruvia/http/http_pmr_string.h"

#include "memory_resource_fixture.h"

using ruvia::test::counting_memory_resource;

TEST(Ruvia, pmr_string_clear_returns_large_storage_and_retains_small_storage) {
    counting_memory_resource resource;
    {
        std::pmr::string buffer(&resource);
        const auto empty_bytes = resource.live_bytes();
        buffer.resize(8192);
        EXPECT_EQ(buffer.size(), std::size_t{8192});
        EXPECT_TRUE(resource.live_bytes() > empty_bytes);
        ruvia::http_clear_pmr_string_retaining_small(buffer);
        EXPECT_TRUE(buffer.empty());
        EXPECT_EQ(resource.live_bytes(), empty_bytes);

        buffer.resize(32);
        const auto retained = resource.live_bytes();
        EXPECT_TRUE(retained > empty_bytes);
        ruvia::http_clear_pmr_string_retaining_small(buffer);
        EXPECT_EQ(resource.live_bytes(), retained);
    }
    EXPECT_EQ(resource.live_bytes(), std::size_t{0});
}
