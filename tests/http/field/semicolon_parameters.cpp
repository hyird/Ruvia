#include "test_harness.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/field/HttpExpectations.h"

namespace {

using ruvia::detail::httpFindSemicolonParameterIgnoreCase;
using ruvia::detail::httpFindSemicolonParameterQuotedIgnoreCase;
using ruvia::detail::httpClientExpectationIsValid;
using ruvia::detail::HttpConnectionOptions;
using ruvia::detail::HttpFieldListParseStatus;
using ruvia::detail::HttpFieldListRole;
using ruvia::detail::HttpRequestContentIndication;
using ruvia::detail::HttpRequestExpectations;
using ruvia::detail::HttpUnsupportedExpectationPolicy;
using ruvia::detail::HttpUpgradeProtocols;

}  // namespace

// Finding a parameter in a semicolon-delimited field value.

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

RUVIA_TEST(find_semicolon_parameter_is_case_sensitive_and_whole_name) {
    using ruvia::detail::httpFindSemicolonParameter;
    // This plain finder backs cookie lookup: cookie names are case-SENSITIVE
    // (RFC 6265), unlike the case-insensitive media-type variants. "sid" and
    // "SID" are distinct keys.
    RUVIA_CHECK_EQ(httpFindSemicolonParameter("sid=1; SID=2", "sid").value_or(""),
                   std::string_view("1"));
    RUVIA_CHECK_EQ(httpFindSemicolonParameter("sid=1; SID=2", "SID").value_or(""),
                   std::string_view("2"));
    // OWS around '=' is trimmed; a value may itself contain '='.
    RUVIA_CHECK_EQ(httpFindSemicolonParameter("theme = dark", "theme").value_or(""),
                   std::string_view("dark"));
    RUVIA_CHECK_EQ(httpFindSemicolonParameter("data=a=b", "data").value_or(""),
                   std::string_view("a=b"));
    // An empty value is present (not absent); a valueless item is skipped entirely.
    RUVIA_CHECK(httpFindSemicolonParameter("flag=", "flag") == std::optional<std::string_view>(""));
    RUVIA_CHECK(!httpFindSemicolonParameter("flag", "flag").has_value());
    // Whole-name match only: a decoy sharing a prefix or suffix must not match, so a
    // "xsid=" can never be read as "sid" (cookie confusion).
    RUVIA_CHECK(!httpFindSemicolonParameter("xsid=evil", "sid").has_value());
    RUVIA_CHECK(!httpFindSemicolonParameter("sidx=evil", "sid").has_value());
}
