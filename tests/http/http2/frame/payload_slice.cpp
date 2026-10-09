#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "http2/http2_payload_slice.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_slice_two_part_payload;

// Virtual concatenation "ABCDE" + "12345" == "ABCDE12345".
constexpr std::string_view first = "ABCDE";
constexpr std::string_view second = "12345";

}  // namespace

RUVIA_TEST(payload_slice_entirely_within_first) {
    const auto slice = http2_slice_two_part_payload(first, second, 0, 3);
    RUVIA_CHECK_EQ(slice.first_, std::string_view("ABC"));
    RUVIA_CHECK(slice.second_.empty());
}

RUVIA_TEST(payload_slice_exactly_first) {
    const auto slice = http2_slice_two_part_payload(first, second, 0, 5);
    RUVIA_CHECK_EQ(slice.first_, std::string_view("ABCDE"));
    RUVIA_CHECK(slice.second_.empty());
}

RUVIA_TEST(payload_slice_spans_the_boundary) {
    const auto slice = http2_slice_two_part_payload(first, second, 0, 7);
    RUVIA_CHECK_EQ(slice.first_, std::string_view("ABCDE"));
    RUVIA_CHECK_EQ(slice.second_, std::string_view("12"));

    // Mid-first offset that also crosses into second.
    const auto mid = http2_slice_two_part_payload(first, second, 3, 4);
    RUVIA_CHECK_EQ(mid.first_, std::string_view("DE"));
    RUVIA_CHECK_EQ(mid.second_, std::string_view("12"));

    // The full concatenation.
    const auto whole = http2_slice_two_part_payload(first, second, 0, 10);
    RUVIA_CHECK_EQ(whole.first_, std::string_view("ABCDE"));
    RUVIA_CHECK_EQ(whole.second_, std::string_view("12345"));
}

RUVIA_TEST(payload_slice_entirely_within_second) {
    const auto at_start = http2_slice_two_part_payload(first, second, 5, 3);
    RUVIA_CHECK_EQ(at_start.first_, std::string_view("123"));
    RUVIA_CHECK(at_start.second_.empty());

    const auto mid_second = http2_slice_two_part_payload(first, second, 7, 2);
    RUVIA_CHECK_EQ(mid_second.first_, std::string_view("34"));
    RUVIA_CHECK(mid_second.second_.empty());
}

RUVIA_TEST(payload_slice_zero_size_is_empty) {
    const auto in_first = http2_slice_two_part_payload(first, second, 0, 0);
    RUVIA_CHECK(in_first.first_.empty());
    RUVIA_CHECK(in_first.second_.empty());

    const auto in_second = http2_slice_two_part_payload(first, second, 5, 0);
    RUVIA_CHECK(in_second.first_.empty());
    RUVIA_CHECK(in_second.second_.empty());
}
