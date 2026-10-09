#include <string_view>

#include "ruvia/http/http_media_type.h"

#include "test_harness.h"

namespace {

using ruvia::http_media_type_only;

}  // namespace

RUVIA_TEST(http_media_type_only_strips_parameters_and_ows) {
    RUVIA_CHECK_EQ(http_media_type_only("application/json"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(
        http_media_type_only("application/json; charset=utf-8"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(http_media_type_only("application/json ; charset=utf-8"),
        std::string_view("application/json"));
    RUVIA_CHECK_EQ(http_media_type_only(""), std::string_view(""));
}

RUVIA_TEST(http_content_type_field_validation_checks_syntax) {
    RUVIA_CHECK(ruvia::is_valid_http_content_type_field_value("text/plain; charset=utf-8"));
    RUVIA_CHECK(!ruvia::is_valid_http_content_type_field_value("text/plain; charset="));
}
