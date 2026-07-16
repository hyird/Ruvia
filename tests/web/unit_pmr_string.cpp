#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/PmrString.h"

namespace {

using ruvia::detail::clearPmrStringRetainingSmall;
using ruvia::detail::compactConsumedPrefix;

std::pmr::string make(std::string_view value) {
    std::pmr::string out(std::pmr::get_default_resource());
    out.assign(value.data(), value.size());
    return out;
}

}  // namespace

RUVIA_TEST(compact_consumed_prefix_clears_when_fully_consumed) {
    auto buffer = make("hello");
    std::size_t offset = 5;  // offset == size
    compactConsumedPrefix(buffer, offset, 2);
    RUVIA_CHECK(buffer.empty());
    RUVIA_CHECK_EQ(offset, std::size_t{0});

    auto over = make("hi");
    std::size_t past = 10;  // offset past the end
    compactConsumedPrefix(over, past, 2);
    RUVIA_CHECK(over.empty());
    RUVIA_CHECK_EQ(past, std::size_t{0});
}

RUVIA_TEST(compact_consumed_prefix_is_lazy_below_threshold) {
    auto buffer = make("hello world");
    std::size_t offset = 3;  // below the compaction threshold -> no move
    compactConsumedPrefix(buffer, offset, 100);
    RUVIA_CHECK_EQ(std::string_view(buffer), std::string_view("hello world"));
    RUVIA_CHECK_EQ(offset, std::size_t{3});
}

RUVIA_TEST(compact_consumed_prefix_moves_tail_at_threshold) {
    auto buffer = make("PREFIXtail");  // 6-byte consumed prefix + "tail"
    std::size_t offset = 6;            // >= threshold
    compactConsumedPrefix(buffer, offset, 4);
    RUVIA_CHECK_EQ(std::string_view(buffer), std::string_view("tail"));  // tail moved to front
    RUVIA_CHECK_EQ(offset, std::size_t{0});
    // A further compaction of the compacted buffer is stable.
    std::size_t zero = 0;
    compactConsumedPrefix(buffer, zero, 4);
    RUVIA_CHECK_EQ(std::string_view(buffer), std::string_view("tail"));
}

RUVIA_TEST(clear_pmr_string_releases_large_capacity) {
    auto buffer = make(std::string(10000, 'x'));  // capacity well above the retained size
    clearPmrStringRetainingSmall(buffer, 4096);
    RUVIA_CHECK(buffer.empty());
    RUVIA_CHECK(buffer.capacity() <= 4096);  // oversized capacity is released

    // A buffer within the retained size is just cleared.
    auto small = make("short");
    clearPmrStringRetainingSmall(small, 4096);
    RUVIA_CHECK(small.empty());
}

RUVIA_TEST(clear_pmr_string_retains_heap_buffer_below_threshold) {
    // The helper's purpose (vs a swap that always frees) is that a HEAP buffer below
    // the retained threshold is KEPT for reuse -- avoiding reallocation churn on the
    // hot connection-reuse path. The "short" case above is small-string-optimized, so
    // it cannot demonstrate a heap buffer surviving. Use a string large enough to be
    // heap allocated but well under the threshold: its capacity must be unchanged.
    auto buffer = make(std::string(200, 'y'));  // heap (> SSO), well under 4096
    const auto capacityBefore = buffer.capacity();
    RUVIA_CHECK(capacityBefore >= 200);
    clearPmrStringRetainingSmall(buffer, 4096);
    RUVIA_CHECK(buffer.empty());
    RUVIA_CHECK_EQ(buffer.capacity(), capacityBefore);  // retained for reuse, not released
}
