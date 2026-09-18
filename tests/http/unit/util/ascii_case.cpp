#include <string_view>

#include "ruvia/http/detail/util/AsciiCase.h"

#include "test_harness.h"

namespace {

using ruvia::detail::httpAsciiEqualsIgnoreCase;

}  // namespace

RUVIA_TEST(http_ascii_equals_ignore_case_folds_only_letters) {
    RUVIA_CHECK(httpAsciiEqualsIgnoreCase("Text/HTML", "text/html"));
    RUVIA_CHECK(httpAsciiEqualsIgnoreCase("", ""));
    RUVIA_CHECK(!httpAsciiEqualsIgnoreCase("abc", "abcd"));
    RUVIA_CHECK(!httpAsciiEqualsIgnoreCase("abc", "abd"));

    RUVIA_CHECK(!httpAsciiEqualsIgnoreCase("[", "{"));
    RUVIA_CHECK(!httpAsciiEqualsIgnoreCase("@", "`"));

    RUVIA_CHECK(httpAsciiEqualsIgnoreCase(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\xA9", 2)));
    RUVIA_CHECK(!httpAsciiEqualsIgnoreCase(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\x89", 2)));
}
