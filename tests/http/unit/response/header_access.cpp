#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/response/http_response_header_bits.h"

#include "response/http_response_header_access.h"
#include "test_harness.h"

namespace {

using ruvia::http_response_header;
using ruvia::detail::make_response_header;
using ruvia::detail::response_header_append;
using ruvia::detail::response_header_known_bit;
using ruvia::detail::response_header_value_begin;
using ruvia::detail::response_header_value_end;
using ruvia::detail::set_response_header_append;

}  // namespace

RUVIA_TEST(response_header_make_splits_name_and_value) {
    const char blob[] = "Content-Typetext/html";  // 12-byte name + 9-byte value
    const auto header_value =
        make_response_header(blob, 12, 9, ruvia::detail::response_header_content_type, false);
    RUVIA_CHECK_EQ(header_value.name(), std::string_view("Content-Type"));
    RUVIA_CHECK_EQ(header_value.value(), std::string_view("text/html"));
    RUVIA_CHECK_EQ(response_header_known_bit(header_value), ruvia::detail::response_header_content_type);
    RUVIA_CHECK(!response_header_append(header_value));  // make() starts with append=false
}

RUVIA_TEST(response_header_value_range_points_into_blob) {
    char blob[] = "X-Fooabcde";  // 5-byte name + 5-byte value
    auto header_value = make_response_header(blob, 5, 5, 0, false);
    auto* begin = response_header_value_begin(header_value);
    auto* end = response_header_value_end(header_value);
    RUVIA_CHECK(begin != nullptr);
    RUVIA_CHECK(end - begin == 5);
    RUVIA_CHECK(begin == blob + 5);  // in-place editable value region
    RUVIA_CHECK_EQ(
        std::string_view(begin, static_cast<std::size_t>(end - begin)), std::string_view("abcde"));
}

RUVIA_TEST(response_header_null_bytes_yield_null_ranges) {
    auto header_value = make_response_header(nullptr, 0, 0, 0, false);
    RUVIA_CHECK(response_header_value_begin(header_value) == nullptr);
    RUVIA_CHECK(response_header_value_end(header_value) == nullptr);
    RUVIA_CHECK(header_value.name().empty());
    RUVIA_CHECK(header_value.value().empty());
}

RUVIA_TEST(response_header_append_flag_round_trip) {
    const char blob[] = "Set-Cookiek=v";  // 10-byte name + 3-byte value
    auto header_value = make_response_header(blob, 10, 3, 0, false);
    RUVIA_CHECK(!response_header_append(header_value));
    set_response_header_append(header_value, true);
    RUVIA_CHECK(response_header_append(header_value));  // marks a multi-value (appended) header
    set_response_header_append(header_value, false);
    RUVIA_CHECK(!response_header_append(header_value));
}
