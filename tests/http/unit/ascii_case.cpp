#include <gtest/gtest.h>

#include <string_view>

#include "ruvia/http/http_ascii.h"

namespace {

using ruvia::http_ascii_equals_ignore_case;

}  // namespace

TEST(Ruvia, http_ascii_equals_ignore_case_folds_only_letters) {
    EXPECT_TRUE(http_ascii_equals_ignore_case("Text/HTML", "text/html"));
    EXPECT_TRUE(http_ascii_equals_ignore_case("", ""));
    EXPECT_TRUE(!http_ascii_equals_ignore_case("abc", "abcd"));
    EXPECT_TRUE(!http_ascii_equals_ignore_case("abc", "abd"));

    EXPECT_TRUE(!http_ascii_equals_ignore_case("[", "{"));
    EXPECT_TRUE(!http_ascii_equals_ignore_case("@", "`"));

    EXPECT_TRUE(http_ascii_equals_ignore_case(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\xA9", 2)));
    EXPECT_TRUE(!http_ascii_equals_ignore_case(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\x89", 2)));
}
