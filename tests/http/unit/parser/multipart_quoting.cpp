#include <string_view>

#include "ruvia/http/multipart_parser.h"

#include "test_harness.h"

// Quoted names and boundaries in a multipart body, and the delimiter terminator each form requires.

RUVIA_TEST(multipart_boundary_quoted_with_mime_special) {
    const std::string_view content_type_value = R"(multipart/form-data; boundary="a:b")";
    const auto result_value = ruvia::parse_multipart_boundary(content_type_value);
    RUVIA_CHECK(result_value.boundary() != nullptr);
    RUVIA_CHECK_EQ(result_value.boundary()->value(), std::string_view("a:b"));
}
