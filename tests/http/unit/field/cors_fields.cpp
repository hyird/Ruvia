#include <string_view>

#include "ruvia/http/HttpCorsFields.h"

#include "test_harness.h"

RUVIA_TEST(http_cors_request_headers_cursor_preserves_fields_and_skips_empty_members) {
    ruvia::HttpCorsRequestHeaderNames names(" , X-One,\tX-Two\t, ");
    RUVIA_CHECK_EQ(names.next().value(), std::string_view("X-One"));
    RUVIA_CHECK_EQ(names.next().value(), std::string_view("X-Two"));
    RUVIA_CHECK(!names.next());
    RUVIA_CHECK(names.valid());
}

RUVIA_TEST(http_cors_request_headers_cursor_rejects_empty_and_invalid_lists) {
    ruvia::HttpCorsRequestHeaderNames empty(" , \t, ");
    RUVIA_CHECK(!empty.next());
    RUVIA_CHECK(!empty.valid());

    ruvia::HttpCorsRequestHeaderNames invalid("X-Ok, bad name");
    RUVIA_CHECK_EQ(invalid.next().value(), std::string_view("X-Ok"));
    RUVIA_CHECK(!invalid.next());
    RUVIA_CHECK(!invalid.valid());
}
