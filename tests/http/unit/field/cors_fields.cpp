#include <string_view>

#include "field/HttpCorsFields.h"
#include "test_harness.h"

RUVIA_TEST(http_cors_request_headers_requires_nonempty_field_name_list) {
    for (const auto value : {"X-One", " , X-One,\tX-Two\t, "}) {
        RUVIA_CHECK(ruvia::detail::isValidHttpCorsRequestHeaderNames(value));
    }
    for (const auto value : {"", " , \t, ", "X-Ok, bad name", "\"X-Quoted\"", "X-One;parameter", "X-One, bad\r\nname"}) {
        RUVIA_CHECK(!ruvia::detail::isValidHttpCorsRequestHeaderNames(value));
    }
}
