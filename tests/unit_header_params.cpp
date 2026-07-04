#include "test_harness.h"

#include <string_view>

#include "http/HeaderTokenUtils.h"

namespace {

using ruvia::detail::httpFindSemicolonParameterQuotedIgnoreCase;
using ruvia::detail::httpUpdateExpectContinueFlag;

}  // namespace

RUVIA_TEST(expect_continue_flag) {
    bool flag = false;
    RUVIA_CHECK(httpUpdateExpectContinueFlag("100-continue", flag));
    RUVIA_CHECK(flag);

    // Case-insensitive and OWS-tolerant.
    bool flag2 = false;
    RUVIA_CHECK(httpUpdateExpectContinueFlag("  100-Continue  ", flag2));
    RUVIA_CHECK(flag2);

    // Anything else leaves the flag untouched.
    bool flag3 = false;
    RUVIA_CHECK(!httpUpdateExpectContinueFlag("100-continue-extra", flag3));
    RUVIA_CHECK(!flag3);
    RUVIA_CHECK(!httpUpdateExpectContinueFlag("other", flag3));
    RUVIA_CHECK(!flag3);
}

RUVIA_TEST(find_semicolon_parameter_quoted_ignore_case) {
    // Extract a media-type parameter, matching the key case-insensitively.
    const auto boundary = httpFindSemicolonParameterQuotedIgnoreCase(
        "multipart/form-data; boundary=xyz", "boundary");
    RUVIA_CHECK(boundary.has_value());
    RUVIA_CHECK_EQ(*boundary, std::string_view("xyz"));

    const auto charset = httpFindSemicolonParameterQuotedIgnoreCase(
        "text/html; CHARSET=utf-8", "charset");
    RUVIA_CHECK(charset.has_value());
    RUVIA_CHECK_EQ(*charset, std::string_view("utf-8"));

    // A quoted value keeps an embedded ';' rather than splitting on it.
    const auto quoted = httpFindSemicolonParameterQuotedIgnoreCase(
        "form-data; name=\"a;b\"", "name");
    RUVIA_CHECK(quoted.has_value());
    RUVIA_CHECK_EQ(*quoted, std::string_view("\"a;b\""));

    // Absent parameter -> nullopt.
    RUVIA_CHECK(!httpFindSemicolonParameterQuotedIgnoreCase("text/html", "charset").has_value());
}
