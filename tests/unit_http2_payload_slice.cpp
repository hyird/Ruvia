#include "test_harness.h"

#include <cstddef>
#include <string_view>

#include "net/http2/Http2PayloadSlice.h"

namespace {

using ruvia::detail::http2SliceTwoPartPayload;

// Virtual concatenation "ABCDE" + "12345" == "ABCDE12345".
constexpr std::string_view kFirst = "ABCDE";
constexpr std::string_view kSecond = "12345";

}  // namespace

RUVIA_TEST(payload_slice_entirely_within_first) {
    const auto slice = http2SliceTwoPartPayload(kFirst, kSecond, 0, 3);
    RUVIA_CHECK_EQ(slice.first, std::string_view("ABC"));
    RUVIA_CHECK(slice.second.empty());
}

RUVIA_TEST(payload_slice_exactly_first) {
    const auto slice = http2SliceTwoPartPayload(kFirst, kSecond, 0, 5);
    RUVIA_CHECK_EQ(slice.first, std::string_view("ABCDE"));
    RUVIA_CHECK(slice.second.empty());
}

RUVIA_TEST(payload_slice_spans_the_boundary) {
    const auto slice = http2SliceTwoPartPayload(kFirst, kSecond, 0, 7);
    RUVIA_CHECK_EQ(slice.first, std::string_view("ABCDE"));
    RUVIA_CHECK_EQ(slice.second, std::string_view("12"));

    // Mid-first offset that also crosses into second.
    const auto mid = http2SliceTwoPartPayload(kFirst, kSecond, 3, 4);
    RUVIA_CHECK_EQ(mid.first, std::string_view("DE"));
    RUVIA_CHECK_EQ(mid.second, std::string_view("12"));

    // The full concatenation.
    const auto whole = http2SliceTwoPartPayload(kFirst, kSecond, 0, 10);
    RUVIA_CHECK_EQ(whole.first, std::string_view("ABCDE"));
    RUVIA_CHECK_EQ(whole.second, std::string_view("12345"));
}

RUVIA_TEST(payload_slice_entirely_within_second) {
    const auto atStart = http2SliceTwoPartPayload(kFirst, kSecond, 5, 3);
    RUVIA_CHECK_EQ(atStart.first, std::string_view("123"));
    RUVIA_CHECK(atStart.second.empty());

    const auto midSecond = http2SliceTwoPartPayload(kFirst, kSecond, 7, 2);
    RUVIA_CHECK_EQ(midSecond.first, std::string_view("34"));
    RUVIA_CHECK(midSecond.second.empty());
}

RUVIA_TEST(payload_slice_zero_size_is_empty) {
    const auto inFirst = http2SliceTwoPartPayload(kFirst, kSecond, 0, 0);
    RUVIA_CHECK(inFirst.first.empty());
    RUVIA_CHECK(inFirst.second.empty());

    const auto inSecond = http2SliceTwoPartPayload(kFirst, kSecond, 5, 0);
    RUVIA_CHECK(inSecond.first.empty());
    RUVIA_CHECK(inSecond.second.empty());
}
