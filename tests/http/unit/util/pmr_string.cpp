#include "util/pmr_string.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "test_harness.h"

namespace {

using ruvia::detail::clear_pmr_string_retaining_small;
using ruvia::detail::compact_consumed_prefix;

std::pmr::string make(std::string_view value) {
    std::pmr::string out(std::pmr::get_default_resource());
    out.assign(value.data(), value.size());
    return out;
}

}  // namespace

RUVIA_TEST(compact_consumed_prefix_clears_when_fully_consumed) {
    auto buffer = make("hello");
    std::size_t offset = 5;  // offset == size
    compact_consumed_prefix(buffer, offset, 2);
    RUVIA_CHECK(buffer.empty());
    RUVIA_CHECK_EQ(offset, std::size_t{0});

    auto over = make("hi");
    std::size_t past = 10;  // offset past the end
    compact_consumed_prefix(over, past, 2);
    RUVIA_CHECK(over.empty());
    RUVIA_CHECK_EQ(past, std::size_t{0});
}

RUVIA_TEST(compact_consumed_prefix_is_lazy_below_threshold) {
    auto buffer = make("hello world");
    std::size_t offset = 3;  // below the compaction threshold -> no move
    compact_consumed_prefix(buffer, offset, 100);
    RUVIA_CHECK_EQ(std::string_view(buffer), std::string_view("hello world"));
    RUVIA_CHECK_EQ(offset, std::size_t{3});
}

RUVIA_TEST(compact_consumed_prefix_moves_tail_at_threshold) {
    auto buffer = make("PREFIXtail");  // 6-byte consumed prefix + "tail"
    std::size_t offset = 6;            // >= threshold
    compact_consumed_prefix(buffer, offset, 4);
    RUVIA_CHECK_EQ(std::string_view(buffer), std::string_view("tail"));  // tail moved to front
    RUVIA_CHECK_EQ(offset, std::size_t{0});
    // A further compaction of the compacted buffer is stable.
    std::size_t zero = 0;
    compact_consumed_prefix(buffer, zero, 4);
    RUVIA_CHECK_EQ(std::string_view(buffer), std::string_view("tail"));
}

RUVIA_TEST(clear_pmr_string_releases_large_capacity) {
    auto buffer = make(std::string(10000, 'x'));  // capacity well above the retained size
    clear_pmr_string_retaining_small(buffer, 4096);
    RUVIA_CHECK(buffer.empty());
    RUVIA_CHECK(buffer.capacity() <= 4096);  // oversized capacity is released

    // A buffer within the retained size is just cleared.
    auto small = make("short");
    clear_pmr_string_retaining_small(small, 4096);
    RUVIA_CHECK(small.empty());
}

RUVIA_TEST(clear_pmr_string_retains_heap_buffer_below_threshold) {
    // The helper's purpose (vs a swap that always frees) is that a HEAP buffer below
    // the retained threshold is KEPT for reuse -- avoiding reallocation churn on the
    // hot connection-reuse path. The "short" case above is small-string-optimized, so
    // it cannot demonstrate a heap buffer surviving. Use a string large enough to be
    // heap allocated but well under the threshold: its capacity must be unchanged.
    auto buffer = make(std::string(200, 'y'));  // heap (> SSO), well under 4096
    const auto capacity_before = buffer.capacity();
    RUVIA_CHECK(capacity_before >= 200);
    clear_pmr_string_retaining_small(buffer, 4096);
    RUVIA_CHECK(buffer.empty());
    RUVIA_CHECK_EQ(buffer.capacity(), capacity_before);  // retained for reuse, not released
}
