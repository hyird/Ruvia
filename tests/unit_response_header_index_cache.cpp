#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <limits>

#include "http/ResponseHeaderIndexCache.h"

namespace {

using ruvia::detail::recordResponseHeaderIndex;
using ruvia::detail::ResponseHeaderIndexCache;
using ruvia::detail::responseHeaderIndexSlotHasValue;
using ruvia::detail::responseHeaderIndexSlotOverflowed;
using ruvia::detail::responseHeaderIndexSlotValue;

}  // namespace

RUVIA_TEST(response_header_index_cache_records_and_reads) {
    ResponseHeaderIndexCache<8> cache{};  // zero-initialized -> all slots missing
    RUVIA_CHECK(!responseHeaderIndexSlotHasValue(cache[3]));

    recordResponseHeaderIndex(cache, 3, 5);
    RUVIA_CHECK(responseHeaderIndexSlotHasValue(cache[3]));
    RUVIA_CHECK_EQ(responseHeaderIndexSlotValue(cache[3]), std::size_t{5});

    // Index 0 must be distinguishable from "missing" (the +1 slot encoding).
    recordResponseHeaderIndex(cache, 1, 0);
    RUVIA_CHECK(responseHeaderIndexSlotHasValue(cache[1]));
    RUVIA_CHECK_EQ(responseHeaderIndexSlotValue(cache[1]), std::size_t{0});
}

RUVIA_TEST(response_header_index_cache_first_write_wins) {
    ResponseHeaderIndexCache<8> cache{};
    recordResponseHeaderIndex(cache, 2, 4);
    recordResponseHeaderIndex(cache, 2, 9);  // a second write must not overwrite
    RUVIA_CHECK_EQ(responseHeaderIndexSlotValue(cache[2]), std::size_t{4});
}

RUVIA_TEST(response_header_index_cache_overflow_boundary) {
    constexpr auto kMax = static_cast<std::size_t>(std::numeric_limits<std::int16_t>::max());
    // The largest representable index is kMax - 1 (it stores as kMax after the +1).
    ResponseHeaderIndexCache<4> representable{};
    recordResponseHeaderIndex(representable, 0, kMax - 1);
    RUVIA_CHECK(responseHeaderIndexSlotHasValue(representable[0]));
    RUVIA_CHECK_EQ(responseHeaderIndexSlotValue(representable[0]), kMax - 1);
    // An index at/above the max records the overflow sentinel instead of wrapping.
    ResponseHeaderIndexCache<4> overflow{};
    recordResponseHeaderIndex(overflow, 0, kMax);
    RUVIA_CHECK(responseHeaderIndexSlotOverflowed(overflow[0]));
    RUVIA_CHECK(!responseHeaderIndexSlotHasValue(overflow[0]));
}

RUVIA_TEST(response_header_index_cache_out_of_range_slot_is_noop) {
    ResponseHeaderIndexCache<4> cache{};
    recordResponseHeaderIndex(cache, 100, 1);  // slot >= size: must not write out of bounds
    for (const auto slot : cache) {
        RUVIA_CHECK(!responseHeaderIndexSlotHasValue(slot));
    }
}
