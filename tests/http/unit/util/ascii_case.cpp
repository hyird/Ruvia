#include <string_view>

#include "ruvia/http/http_ascii.h"

#include "test_harness.h"

namespace {

using ruvia::http_ascii_equals_ignore_case;

}  // namespace

RUVIA_TEST(http_ascii_equals_ignore_case_folds_only_letters) {
    RUVIA_CHECK(http_ascii_equals_ignore_case("Text/HTML", "text/html"));
    RUVIA_CHECK(http_ascii_equals_ignore_case("", ""));
    RUVIA_CHECK(!http_ascii_equals_ignore_case("abc", "abcd"));
    RUVIA_CHECK(!http_ascii_equals_ignore_case("abc", "abd"));

    RUVIA_CHECK(!http_ascii_equals_ignore_case("[", "{"));
    RUVIA_CHECK(!http_ascii_equals_ignore_case("@", "`"));

    RUVIA_CHECK(http_ascii_equals_ignore_case(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\xA9", 2)));
    RUVIA_CHECK(!http_ascii_equals_ignore_case(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\x89", 2)));
}
