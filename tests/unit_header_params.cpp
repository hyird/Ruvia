#include "test_harness.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpConnectionFields.h"
#include "ruvia/http/detail/HttpExpectations.h"

namespace {

using ruvia::detail::httpFindSemicolonParameterIgnoreCase;
using ruvia::detail::httpFindSemicolonParameterQuotedIgnoreCase;
using ruvia::detail::HttpConnectionOptions;
using ruvia::detail::HttpFieldListParseStatus;
using ruvia::detail::HttpFieldListRole;
using ruvia::detail::HttpRequestContentIndication;
using ruvia::detail::HttpRequestExpectations;
using ruvia::detail::HttpServerExpectationAction;

// {close, keepAlive, upgrade, te} after recipient-side parsing.
std::array<bool, 4> connectionOptions(std::string_view value) {
    HttpConnectionOptions options;
    if (options.parseField(value, HttpFieldListRole::kRecipient) !=
        HttpFieldListParseStatus::kOk) {
        throw std::runtime_error("test expected valid Connection options");
    }
    return {
        options.close(),
        options.keepAlive(),
        options.upgrade(),
        options.te()};
}

}  // namespace

RUVIA_TEST(expectations_parse_one_logical_recipient_list) {
    HttpRequestExpectations expectations;
    expectations.parseField(" , 100-continue, , 100-Continue, ");
    expectations.parseField(" 100-CONTINUE ");

    RUVIA_CHECK(expectations.has100Continue());
    RUVIA_CHECK(!expectations.hasUnsupported());
    RUVIA_CHECK(
        expectations.serverAction(HttpRequestContentIndication::kNone) ==
        HttpServerExpectationAction::kNone);
    RUVIA_CHECK(
        expectations.serverAction(HttpRequestContentIndication::kWillFollow) ==
        HttpServerExpectationAction::kSend100Continue);
}

RUVIA_TEST(expectations_preserve_unsupported_extensions_as_semantics) {
    HttpRequestExpectations expectations;
    expectations.parseField("100-continue");
    expectations.parseField(R"(custom="a,b")");

    RUVIA_CHECK(expectations.has100Continue());
    RUVIA_CHECK(expectations.hasUnsupported());
    RUVIA_CHECK(
        expectations.serverAction(HttpRequestContentIndication::kWillFollow) ==
        HttpServerExpectationAction::kUnsupported);

    expectations.ignore100Continue();
    RUVIA_CHECK(!expectations.has100Continue());
    RUVIA_CHECK(expectations.hasUnsupported());
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

RUVIA_TEST(connection_options_parse_tokens_case_insensitively) {
    using Arr = std::array<bool, 4>;  // {close, keepAlive, upgrade, te}

    // Single tokens, matched case-insensitively.
    RUVIA_CHECK((connectionOptions("close") == Arr{true, false, false, false}));
    RUVIA_CHECK((connectionOptions("CLOSE") == Arr{true, false, false, false}));
    RUVIA_CHECK((connectionOptions("keep-alive") == Arr{false, true, false, false}));
    RUVIA_CHECK((connectionOptions("Keep-Alive") == Arr{false, true, false, false}));
    RUVIA_CHECK((connectionOptions("Upgrade") == Arr{false, false, true, false}));
    RUVIA_CHECK((connectionOptions("UPGRADE") == Arr{false, false, true, false}));

    // A comma list sets each recognised token; OWS around tokens is trimmed.
    RUVIA_CHECK((connectionOptions("keep-alive, Upgrade") == Arr{false, true, true, false}));
    RUVIA_CHECK((connectionOptions("close , upgrade") == Arr{true, false, true, false}));
    RUVIA_CHECK((connectionOptions("close, keep-alive, upgrade") == Arr{true, true, true, false}));

    // Empty list items (leading / trailing / doubled comma) are skipped, not fatal.
    RUVIA_CHECK((connectionOptions(",close") == Arr{true, false, false, false}));
    RUVIA_CHECK((connectionOptions("close,") == Arr{true, false, false, false}));
    RUVIA_CHECK((connectionOptions("keep-alive,,upgrade") == Arr{false, true, true, false}));

    // Unrecognised tokens are ignored; a recognised neighbour still registers.
    RUVIA_CHECK((connectionOptions("TE, close") == Arr{true, false, false, true}));
    RUVIA_CHECK((connectionOptions("x-foo") == Arr{false, false, false, false}));
    RUVIA_CHECK((connectionOptions("") == Arr{false, false, false, false}));
}

RUVIA_TEST(connection_options_enforce_sender_and_recipient_list_roles) {
    for (const auto value : {",close", "close,", "close,,Upgrade", ""}) {
        HttpConnectionOptions sender;
        RUVIA_CHECK(
            sender.parseField(value, HttpFieldListRole::kSender) ==
            HttpFieldListParseStatus::kMalformed);
    }

    HttpConnectionOptions repeated;
    RUVIA_CHECK(
        repeated.parseField("keep-alive", HttpFieldListRole::kSender) ==
        HttpFieldListParseStatus::kOk);
    RUVIA_CHECK(
        repeated.parseField("TE, Upgrade", HttpFieldListRole::kSender) ==
        HttpFieldListParseStatus::kOk);
    RUVIA_CHECK(repeated.keepAlive());
    RUVIA_CHECK(repeated.te());
    RUVIA_CHECK(repeated.upgrade());

    HttpConnectionOptions malformed;
    RUVIA_CHECK(
        malformed.parseField("close;param", HttpFieldListRole::kRecipient) ==
        HttpFieldListParseStatus::kMalformed);
}
