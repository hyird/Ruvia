#include <string_view>

#include "ruvia/http/HttpOrigin.h"

#include "field/HttpOriginFields.h"
#include "test_harness.h"

RUVIA_TEST(http_origin_field_accepts_opaque_origin_and_serialized_origin_lists) {
    for (const auto value : {"null", " \tnull\t ", "https://example.com",
             "https://example.com http://other.example:8080", "\thttps://example.com https://[::1]\t"}) {
        RUVIA_CHECK(ruvia::detail::is_valid_http_origin_field_value(value));
    }
    // A field can contain null or several origins; neither is a single
    // serialized origin usable in configuration or connection advertisements.
    RUVIA_CHECK(!ruvia::is_valid_http_serialized_origin("null"));
    RUVIA_CHECK(!ruvia::is_valid_http_serialized_origin("https://example.com http://other.example"));
}

RUVIA_TEST(http_origin_field_rejects_malformed_lists_and_noncanonical_origins) {
    for (const auto value : {"", " \t ", "null https://example.com", "https://example.com null",
             "https://example.com  http://other.example", "https://example.com\thttp://other.example",
             "https://example.com,http://other.example", "https://example.com/", "https://EXAMPLE.com",
             "https://example.com:443", "https://example.com\r\n"}) {
        RUVIA_CHECK(!ruvia::detail::is_valid_http_origin_field_value(value));
    }
    RUVIA_CHECK(!ruvia::detail::is_valid_http_origin_field_value(std::string_view("https://example.com\0", 20)));
}
