#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "HttpResponseHeaderAccess.h"
#include "HttpResponseHeaderBits.h"

namespace {

using ruvia::HttpResponseHeader;
using ruvia::detail::makeResponseHeader;
using ruvia::detail::responseHeaderAppend;
using ruvia::detail::responseHeaderKnownBit;
using ruvia::detail::responseHeaderValueBegin;
using ruvia::detail::responseHeaderValueEnd;
using ruvia::detail::setResponseHeaderAppend;

}  // namespace

RUVIA_TEST(response_header_make_splits_name_and_value) {
    const char blob[] = "Content-Typetext/html";  // 12-byte name + 9-byte value
    const auto header = makeResponseHeader(blob, 12, 9, ruvia::detail::kResponseHeaderContentType, false);
    RUVIA_CHECK_EQ(header.name(), std::string_view("Content-Type"));
    RUVIA_CHECK_EQ(header.value(), std::string_view("text/html"));
    RUVIA_CHECK_EQ(responseHeaderKnownBit(header), ruvia::detail::kResponseHeaderContentType);
    RUVIA_CHECK(!responseHeaderAppend(header));  // make() starts with append=false
}

RUVIA_TEST(response_header_value_range_points_into_blob) {
    char blob[] = "X-Fooabcde";  // 5-byte name + 5-byte value
    auto header = makeResponseHeader(blob, 5, 5, 0, false);
    auto* begin = responseHeaderValueBegin(header);
    auto* end = responseHeaderValueEnd(header);
    RUVIA_CHECK(begin != nullptr);
    RUVIA_CHECK(end - begin == 5);
    RUVIA_CHECK(begin == blob + 5);  // in-place editable value region
    RUVIA_CHECK_EQ(std::string_view(begin, static_cast<std::size_t>(end - begin)),
                   std::string_view("abcde"));
}

RUVIA_TEST(response_header_null_bytes_yield_null_ranges) {
    auto header = makeResponseHeader(nullptr, 0, 0, 0, false);
    RUVIA_CHECK(responseHeaderValueBegin(header) == nullptr);
    RUVIA_CHECK(responseHeaderValueEnd(header) == nullptr);
    RUVIA_CHECK(header.name().empty());
    RUVIA_CHECK(header.value().empty());
}

RUVIA_TEST(response_header_append_flag_round_trip) {
    const char blob[] = "Set-Cookiek=v";  // 10-byte name + 3-byte value
    auto header = makeResponseHeader(blob, 10, 3, 0, false);
    RUVIA_CHECK(!responseHeaderAppend(header));
    setResponseHeaderAppend(header, true);
    RUVIA_CHECK(responseHeaderAppend(header));  // marks a multi-value (appended) header
    setResponseHeaderAppend(header, false);
    RUVIA_CHECK(!responseHeaderAppend(header));
}
