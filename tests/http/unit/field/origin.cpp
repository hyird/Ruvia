#include <gtest/gtest.h>

#include <string_view>

#include "ruvia/http/http_request_target.h"

TEST(Ruvia, http_serialized_origin_rejects_opaque_origin_and_origin_lists) {
    // A field can contain null or several origins; neither is a single
    // serialized origin usable in configuration or connection advertisements.
    EXPECT_TRUE(!ruvia::is_valid_http_serialized_origin("null"));
    EXPECT_TRUE(!ruvia::is_valid_http_serialized_origin("https://example.com http://other.example"));
}
