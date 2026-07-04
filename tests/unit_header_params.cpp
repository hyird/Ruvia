#include "test_harness.h"

#include <string_view>

#include "http/HeaderTokenUtils.h"

namespace {

using ruvia::detail::httpFindSemicolonParameterIgnoreCase;
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

    // A quoted-pair (\") does not close the quote, so a ';' after it stays
    // inside the value (e.g. a multipart filename containing an escaped quote),
    // and a real parameter following the quoted value is still parsed.
    const std::string_view withPair = "form-data; name=\"a\\\"b;c\"; charset=utf-8";
    const auto pairValue = httpFindSemicolonParameterQuotedIgnoreCase(withPair, "name");
    RUVIA_CHECK(pairValue.has_value());
    RUVIA_CHECK_EQ(*pairValue, std::string_view("\"a\\\"b;c\""));
    const auto trailing = httpFindSemicolonParameterQuotedIgnoreCase(withPair, "charset");
    RUVIA_CHECK(trailing.has_value());
    RUVIA_CHECK_EQ(*trailing, std::string_view("utf-8"));

    // Absent parameter -> nullopt.
    RUVIA_CHECK(!httpFindSemicolonParameterQuotedIgnoreCase("text/html", "charset").has_value());
}

RUVIA_TEST(find_semicolon_parameter_quoted_ignore_case_uses_last_match) {
    const auto charset = httpFindSemicolonParameterQuotedIgnoreCase(
        "text/html; charset=latin1; CHARSET=utf-8", "charset");
    RUVIA_CHECK(charset.has_value());
    RUVIA_CHECK_EQ(*charset, std::string_view("utf-8"));
}

RUVIA_TEST(find_semicolon_parameter_ignore_case_uses_last_match) {
    const auto value = httpFindSemicolonParameterIgnoreCase(
        "token=first; TOKEN=second", "token");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(*value, std::string_view("second"));
}

RUVIA_TEST(find_semicolon_parameter_matches_whole_name_not_substring) {
    // The parameter name is matched as a WHOLE token, never as a substring. A decoy
    // parameter whose name merely contains the sought name as a prefix or suffix must
    // NOT match -- otherwise an attacker could smuggle a boundary or charset value
    // through a differently-named parameter (e.g. a "notboundary=" a substring-based
    // find() would latch onto), steering multipart framing or content decoding.
    RUVIA_CHECK(!httpFindSemicolonParameterQuotedIgnoreCase(
        "multipart/form-data; notboundary=evil", "boundary").has_value());
    RUVIA_CHECK(!httpFindSemicolonParameterQuotedIgnoreCase(
        "multipart/form-data; boundaryx=evil", "boundary").has_value());
    RUVIA_CHECK(!httpFindSemicolonParameterIgnoreCase(
        "text/html; xcharset=evil", "charset").has_value());

    // A genuine parameter is still found even when a decoy substring-name precedes it.
    const auto real = httpFindSemicolonParameterQuotedIgnoreCase(
        "multipart/form-data; notboundary=evil; boundary=real", "boundary");
    RUVIA_CHECK(real.has_value());
    RUVIA_CHECK_EQ(*real, std::string_view("real"));
}
