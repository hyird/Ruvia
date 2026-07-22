#include "test_harness.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "ruvia/http/detail/response/ResponseHeaderIndexCache.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"

namespace {

using ruvia::HttpResponseHeader;
using ruvia::detail::findResponseHeaderIndexed;
using ruvia::detail::kOverflowResponseHeaderIndexSlot;
using ruvia::detail::makeResponseHeader;
using ruvia::detail::recordResponseHeaderIndex;
using ruvia::detail::ResponseHeaderIndexCache;
using ruvia::detail::responseHeaderIndexSlotHasValue;
using ruvia::detail::responseHeaderIndexSlotOverflowed;
using ruvia::detail::responseHeaderIndexSlotValue;

// Three response headers: content-type (known bit), x-custom (no bit), location.
std::array<HttpResponseHeader, 3> sampleHeaders() {
    return {
        makeResponseHeader("content-typetext/html", 12, 9, ruvia::detail::kResponseHeaderContentType, false),
        makeResponseHeader("x-customval", 8, 3, 0, false),
        makeResponseHeader("locationhttps://x", 8, 9, ruvia::detail::kResponseHeaderLocation, false),
    };
}

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

RUVIA_TEST(find_response_header_indexed_cache_hit) {
    const auto headers = sampleHeaders();
    ResponseHeaderIndexCache<8> cache{};
    cache[5] = 1;  // slot 5 -> header index 0 (stored as index + 1)
    // The fast path returns the cached header without scanning (name/bit ignored).
    const auto* found = findResponseHeaderIndexed(
        headers.data(), headers.data() + headers.size(), cache, 5, "", 0);
    RUVIA_CHECK(found == headers.data());
}

RUVIA_TEST(find_response_header_indexed_cache_miss_is_authoritative) {
    const auto headers = sampleHeaders();
    ResponseHeaderIndexCache<8> cache{};  // slot 5 == 0 (missing, not overflow)
    // A missing (non-overflow) slot means "not recorded": end, no scan, even
    // though the header is present in the list.
    const auto* found = findResponseHeaderIndexed(
        headers.data(), headers.data() + headers.size(), cache, 5, "location",
        ruvia::detail::kResponseHeaderLocation);
    RUVIA_CHECK(found == headers.data() + headers.size());
}

RUVIA_TEST(find_response_header_indexed_overflow_scans_by_bit) {
    const auto headers = sampleHeaders();
    ResponseHeaderIndexCache<8> cache{};
    cache[5] = kOverflowResponseHeaderIndexSlot;  // -1 -> fall back to a linear scan
    const auto* found = findResponseHeaderIndexed(
        headers.data(), headers.data() + headers.size(), cache, 5, "",
        ruvia::detail::kResponseHeaderLocation);
    RUVIA_CHECK(found == headers.data() + 2);
}

RUVIA_TEST(find_response_header_indexed_out_of_range_scans_by_name) {
    const auto headers = sampleHeaders();
    ResponseHeaderIndexCache<8> cache{};
    // A slot beyond the cache skips it and scans by case-insensitive name.
    const auto* found = findResponseHeaderIndexed(
        headers.data(), headers.data() + headers.size(), cache, 100, "X-Custom", 0);
    RUVIA_CHECK(found == headers.data() + 1);
    // An absent name yields end.
    const auto* absent = findResponseHeaderIndexed(
        headers.data(), headers.data() + headers.size(), cache, 100, "x-absent", 0);
    RUVIA_CHECK(absent == headers.data() + headers.size());
}
