#include <gtest/gtest.h>

#include <string_view>

#include "ruvia/http/http_field_syntax.h"

TEST(Ruvia, http_field_ows_trims_only_space_and_tab) {
    EXPECT_EQ(ruvia::http_trim_ows(" \t text/plain \t"), std::string_view("text/plain"));
    EXPECT_EQ(ruvia::http_trim_ows("\ntext/plain\n"), std::string_view("\ntext/plain\n"));
}
