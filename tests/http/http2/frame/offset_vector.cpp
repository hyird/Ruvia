#include "test_harness.h"

#include <cstddef>
#include <string>
#include <vector>

#include "ruvia/http/detail/http2/frame/Http2OffsetVector.h"

namespace {

using ruvia::detail::http2CompactMovableOffsetVector;
using ruvia::detail::http2CompactOffsetVector;
using ruvia::detail::http2ShouldCompactOffsetVector;

}  // namespace

RUVIA_TEST(offset_vector_should_compact_decision) {
    // offset 0 -> never compact.
    std::vector<int> zero = {1, 2, 3};
    std::size_t off = 0;
    RUVIA_CHECK(!http2ShouldCompactOffsetVector(zero, off, 2));

    // offset == size -> clear as a side effect and return false.
    std::vector<int> full = {1, 2, 3};
    std::size_t consumed = 3;
    RUVIA_CHECK(!http2ShouldCompactOffsetVector(full, consumed, 2));
    RUVIA_CHECK(full.empty());
    RUVIA_CHECK_EQ(consumed, std::size_t{0});

    // offset >= threshold -> compact.
    std::vector<int> overThreshold = {1, 2, 3, 4, 5};
    std::size_t big = 3;
    RUVIA_CHECK(http2ShouldCompactOffsetVector(overThreshold, big, 2));

    // Below threshold but the consumed prefix is at least half -> compact.
    std::vector<int> halfConsumed = {1, 2, 3, 4, 5};
    std::size_t half = 3;
    RUVIA_CHECK(http2ShouldCompactOffsetVector(halfConsumed, half, 100));

    // Below threshold and a small prefix -> lazy, no compaction.
    std::vector<int> lazy = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    std::size_t small = 2;
    RUVIA_CHECK(!http2ShouldCompactOffsetVector(lazy, small, 100));
}

RUVIA_TEST(offset_vector_compact_trivial_memmove) {
    std::vector<int> values = {10, 20, 30, 40, 50};
    std::size_t offset = 3;  // [10 20 30] consumed, [40 50] remain
    http2CompactOffsetVector(values, offset, 2);
    RUVIA_CHECK_EQ(values.size(), std::size_t{2});
    RUVIA_CHECK_EQ(values[0], 40);
    RUVIA_CHECK_EQ(values[1], 50);
    RUVIA_CHECK_EQ(offset, std::size_t{0});
}

RUVIA_TEST(offset_vector_compact_movable) {
    std::vector<std::string> values = {"a", "b", "c", "d"};
    std::size_t offset = 2;  // "c" "d" remain
    http2CompactMovableOffsetVector(values, offset, 1);
    RUVIA_CHECK_EQ(values.size(), std::size_t{2});
    RUVIA_CHECK_EQ(values[0], std::string("c"));
    RUVIA_CHECK_EQ(values[1], std::string("d"));
    RUVIA_CHECK_EQ(offset, std::size_t{0});

    // Below threshold with a small prefix leaves it untouched.
    std::vector<std::string> lazy = {"x", "y", "z"};
    std::size_t lazyOffset = 1;
    http2CompactMovableOffsetVector(lazy, lazyOffset, 100);
    RUVIA_CHECK_EQ(lazy.size(), std::size_t{3});
    RUVIA_CHECK_EQ(lazyOffset, std::size_t{1});

    // Fully consumed clears.
    std::vector<std::string> drained = {"p", "q"};
    std::size_t drainedOffset = 2;
    http2CompactMovableOffsetVector(drained, drainedOffset, 1);
    RUVIA_CHECK(drained.empty());
    RUVIA_CHECK_EQ(drainedOffset, std::size_t{0});
}

RUVIA_TEST(offset_vector_compact_overlapping_left_shift) {
    // Threshold-triggered compaction where the consumed prefix is SMALLER than
    // the surviving suffix: the move region overlaps its destination. The trivial
    // path must use memmove (not memcpy) and the movable path must read each
    // element before it is overwritten (forward iteration), or the result corrupts.
    std::vector<int> trivial = {10, 20, 30, 40, 50, 60};
    std::size_t off = 2;  // remaining (4) > offset (2) -> overlap; threshold 2 triggers it
    http2CompactOffsetVector(trivial, off, 2);
    RUVIA_CHECK_EQ(trivial.size(), std::size_t{4});
    RUVIA_CHECK_EQ(trivial[0], 30);
    RUVIA_CHECK_EQ(trivial[3], 60);
    RUVIA_CHECK_EQ(off, std::size_t{0});

    std::vector<std::string> movable = {"a", "b", "c", "d", "e"};
    std::size_t moff = 2;  // remaining (3) > offset (2) -> overlap
    http2CompactMovableOffsetVector(movable, moff, 2);
    RUVIA_CHECK_EQ(movable.size(), std::size_t{3});
    RUVIA_CHECK_EQ(movable[0], std::string("c"));
    RUVIA_CHECK_EQ(movable[1], std::string("d"));
    RUVIA_CHECK_EQ(movable[2], std::string("e"));
    RUVIA_CHECK_EQ(moff, std::size_t{0});
}
