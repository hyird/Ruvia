#include <gtest/gtest.h>

#include <string_view>

#include "ruvia/http/http_accept_match.h"

namespace {

using ruvia::http_media_type_only;

}  // namespace

TEST(Ruvia, http_media_type_only_strips_parameters_and_ows) {
    EXPECT_EQ(http_media_type_only("application/json"), std::string_view("application/json"));
    EXPECT_EQ(http_media_type_only("application/json; charset=utf-8"), std::string_view("application/json"));
    EXPECT_EQ(http_media_type_only("application/json ; charset=utf-8"), std::string_view("application/json"));
    EXPECT_EQ(http_media_type_only(""), std::string_view(""));
}

TEST(Ruvia, http_content_type_field_validation_checks_syntax) {
    EXPECT_TRUE(ruvia::is_valid_http_content_type_field_value("text/plain; charset=utf-8"));
    EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value("text/plain; charset="));
}

TEST(Ruvia, http_content_type_allows_empty_parameter_slots) {
    for (const std::string_view field : {
             "text/plain;",
             "text/plain;;",
             "text/plain; \t; charset=utf-8; ;",
             R"(text/plain;;note="a;b";;charset="utf-8";)",
             R"(text/plain;;note="";)",
             "text/plain;q=invalid",
             R"(text/plain;q="0.5")"}) {
        EXPECT_TRUE(ruvia::is_valid_http_content_type_field_value(field));
    }
}

TEST(Ruvia, http_content_type_empty_slots_do_not_relax_nonempty_parameter_syntax) {
    for (const std::string_view field : {
             "text/plain;;charset;",
             "text/plain;;charset=;",
             "text/plain;;=utf-8;",
             "text/plain;;charset =utf-8;",
             "text/plain;;charset= utf-8;",
             "text/plain;;charset=utf-8;;CHARSET=utf-8;",
             "text/plain;;note=\"unterminated;",
             R"(text/plain;note="closed"suffix)",
             "text/plain;note=a/b"}) {
        EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value(field));
    }
}
