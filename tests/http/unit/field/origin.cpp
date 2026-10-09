#include <string_view>

#include "ruvia/http/http_origin.h"

#include "test_harness.h"

RUVIA_TEST(http_serialized_origin_rejects_opaque_origin_and_origin_lists) {
    // A field can contain null or several origins; neither is a single
    // serialized origin usable in configuration or connection advertisements.
    RUVIA_CHECK(!ruvia::is_valid_http_serialized_origin("null"));
    RUVIA_CHECK(!ruvia::is_valid_http_serialized_origin("https://example.com http://other.example"));
}
