#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "ruvia/http/detail/response/http_response_header_bits.h"

#include "response/response_header_index_cache.h"
#include "test_harness.h"

namespace {

using ruvia::http_response_header;
using ruvia::detail::find_response_header_indexed;
using ruvia::detail::make_response_header;
using ruvia::detail::overflow_response_header_index_slot;
using ruvia::detail::record_response_header_index;
using ruvia::detail::response_header_index_cache_type;
using ruvia::detail::response_header_index_slot_has_value;
using ruvia::detail::response_header_index_slot_overflowed;
using ruvia::detail::response_header_index_slot_value;

// Three response headers: content-type (known bit), x-custom (no bit), location.
std::array<http_response_header, 3> sample_headers() {
    return {
        make_response_header(
            "content-typetext/html", 12, 9, ruvia::detail::response_header_content_type, false),
        make_response_header("x-customval", 8, 3, 0, false),
        make_response_header(
            "locationhttps://x", 8, 9, ruvia::detail::response_header_location, false),
    };
}

}  // namespace

RUVIA_TEST(response_header_index_cache_records_and_reads) {
    response_header_index_cache_type<8> cache{};  // zero-initialized -> all slots missing
    RUVIA_CHECK(!response_header_index_slot_has_value(cache[3]));

    record_response_header_index(cache, 3, 5);
    RUVIA_CHECK(response_header_index_slot_has_value(cache[3]));
    RUVIA_CHECK_EQ(response_header_index_slot_value(cache[3]), std::size_t{5});

    // Index 0 must be distinguishable from "missing" (the +1 slot encoding).
    record_response_header_index(cache, 1, 0);
    RUVIA_CHECK(response_header_index_slot_has_value(cache[1]));
    RUVIA_CHECK_EQ(response_header_index_slot_value(cache[1]), std::size_t{0});
}

RUVIA_TEST(response_header_index_cache_first_write_wins) {
    response_header_index_cache_type<8> cache{};
    record_response_header_index(cache, 2, 4);
    record_response_header_index(cache, 2, 9);  // a second write must not overwrite
    RUVIA_CHECK_EQ(response_header_index_slot_value(cache[2]), std::size_t{4});
}

RUVIA_TEST(response_header_index_cache_overflow_boundary) {
    constexpr auto max_value = static_cast<std::size_t>(std::numeric_limits<std::int16_t>::max());
    // The largest representable index is max - 1 (it stores as max after the +1).
    response_header_index_cache_type<8> representable{};
    record_response_header_index(representable, 0, max_value - 1);
    RUVIA_CHECK(response_header_index_slot_has_value(representable[0]));
    RUVIA_CHECK_EQ(response_header_index_slot_value(representable[0]), max_value - 1);
    // An index at/above the max records the overflow sentinel instead of wrapping.
    response_header_index_cache_type<8> overflow{};
    record_response_header_index(overflow, 0, max_value);
    RUVIA_CHECK(response_header_index_slot_overflowed(overflow[0]));
    RUVIA_CHECK(!response_header_index_slot_has_value(overflow[0]));
}

RUVIA_TEST(response_header_index_cache_out_of_range_slot_is_noop) {
    response_header_index_cache_type<8> cache{};
    record_response_header_index(cache, 100, 1);  // slot >= size: must not write out of bounds
    for (const auto slot : cache) {
        RUVIA_CHECK(!response_header_index_slot_has_value(slot));
    }
}

RUVIA_TEST(find_response_header_indexed_cache_hit) {
    const auto headers = sample_headers();
    response_header_index_cache_type<8> cache{};
    cache[5] = 1;  // slot 5 -> header index 0 (stored as index + 1)
    // The fast path returns the cached header without scanning (name/bit ignored).
    const auto* found =
        find_response_header_indexed(headers.data(), headers.data() + headers.size(), cache, 5, "", 0);
    RUVIA_CHECK(found == headers.data());
}

RUVIA_TEST(find_response_header_indexed_cache_miss_is_authoritative) {
    const auto headers = sample_headers();
    response_header_index_cache_type<8> cache{};  // slot 5 == 0 (missing, not overflow)
    // A missing (non-overflow) slot means "not recorded": end, no scan, even
    // though the header is present in the list.
    const auto* found = find_response_header_indexed(headers.data(), headers.data() + headers.size(),
        cache, 5, "location", ruvia::detail::response_header_location);
    RUVIA_CHECK(found == headers.data() + headers.size());
}

RUVIA_TEST(find_response_header_indexed_overflow_scans_by_bit) {
    const auto headers = sample_headers();
    response_header_index_cache_type<8> cache{};
    cache[5] = overflow_response_header_index_slot;  // -1 -> fall back to a linear scan
    const auto* found = find_response_header_indexed(headers.data(), headers.data() + headers.size(),
        cache, 5, "", ruvia::detail::response_header_location);
    RUVIA_CHECK(found == headers.data() + 2);
}

RUVIA_TEST(find_response_header_indexed_out_of_range_scans_by_name) {
    const auto headers = sample_headers();
    response_header_index_cache_type<8> cache{};
    // A slot beyond the cache skips it and scans by case-insensitive name.
    const auto* found = find_response_header_indexed(
        headers.data(), headers.data() + headers.size(), cache, 100, "X-Custom", 0);
    RUVIA_CHECK(found == headers.data() + 1);
    // An absent name yields end.
    const auto* absent = find_response_header_indexed(
        headers.data(), headers.data() + headers.size(), cache, 100, "x-absent", 0);
    RUVIA_CHECK(absent == headers.data() + headers.size());
}
