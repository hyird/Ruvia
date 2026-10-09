#include <string_view>

#include "ruvia/http/http_field_whitespace.h"

#include "test_harness.h"

RUVIA_TEST(http_field_ows_trims_only_space_and_tab) {
    RUVIA_CHECK_EQ(ruvia::http_trim_ows(" \t text/plain \t"), std::string_view("text/plain"));
    RUVIA_CHECK_EQ(ruvia::http_trim_ows("\ntext/plain\n"), std::string_view("\ntext/plain\n"));
}
