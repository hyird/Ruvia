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

RUVIA_TEST(http_content_type_allows_empty_parameter_slots) {
    for (const std::string_view field : {
             "text/plain;",
             "text/plain;;",
             "text/plain; \t; charset=utf-8; ;",
             R"(text/plain;;note="a;b";;charset="utf-8";)",
             R"(text/plain;;note="";)"}) {
        RUVIA_CHECK(ruvia::is_valid_http_content_type_field_value(field));
    }
}

RUVIA_TEST(http_content_type_empty_slots_do_not_relax_nonempty_parameter_syntax) {
    for (const std::string_view field : {
             "text/plain;;charset;",
             "text/plain;;charset=;",
             "text/plain;;=utf-8;",
             "text/plain;;charset =utf-8;",
             "text/plain;;charset= utf-8;",
             "text/plain;;charset=utf-8;;CHARSET=utf-8;",
             "text/plain;;note=\"unterminated;"}) {
        RUVIA_CHECK(!ruvia::is_valid_http_content_type_field_value(field));
    }
}
