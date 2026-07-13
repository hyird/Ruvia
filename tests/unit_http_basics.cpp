#include "test_harness.h"

#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpParseError.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::HttpParseError;
using ruvia::httpParseErrorMessage;
using ruvia::isValidHttpHeaderName;
using ruvia::isValidHttpHeaderValue;
using ruvia::isValidHttpMethodToken;
using ruvia::isValidHttpStatusText;
using ruvia::knownHttpMethodToken;
using ruvia::classifyHttpMethod;

}  // namespace

RUVIA_TEST(http_header_name_validation) {
    RUVIA_CHECK(isValidHttpHeaderName("Content-Type"));
    RUVIA_CHECK(isValidHttpHeaderName("X-Custom-Header"));
    RUVIA_CHECK(isValidHttpHeaderName("a"));
    RUVIA_CHECK(!isValidHttpHeaderName(""));            // empty name is invalid
    RUVIA_CHECK(!isValidHttpHeaderName("X Y"));         // space is not a token char
    RUVIA_CHECK(!isValidHttpHeaderName("X:Y"));         // ':' is a separator
    RUVIA_CHECK(!isValidHttpHeaderName("(comment)"));   // '(' ')' are separators
    RUVIA_CHECK(!isValidHttpHeaderName("a@b"));         // '@' is a separator
    RUVIA_CHECK(!isValidHttpHeaderName(std::string_view("a\rb", 3)));  // control char
}

RUVIA_TEST(http_header_value_rejects_injection) {
    RUVIA_CHECK(isValidHttpHeaderValue("text/html; charset=utf-8"));  // spaces allowed
    RUVIA_CHECK(isValidHttpHeaderValue("a b"));                       // inner spaces allowed
    RUVIA_CHECK(isValidHttpHeaderValue(""));                          // empty value is valid
    RUVIA_CHECK(!isValidHttpHeaderValue(" leading"));
    RUVIA_CHECK(!isValidHttpHeaderValue("trailing "));
    RUVIA_CHECK(!isValidHttpHeaderValue("\ttab"));
    RUVIA_CHECK(!isValidHttpHeaderValue("tab\t"));
    // CR, LF and NUL must be rejected -- these are response/header-injection bytes.
    RUVIA_CHECK(!isValidHttpHeaderValue(std::string_view("a\rb", 3)));
    RUVIA_CHECK(!isValidHttpHeaderValue(std::string_view("a\nb", 3)));
    RUVIA_CHECK(!isValidHttpHeaderValue(std::string_view("a\r\nInjected: x", 14)));
    RUVIA_CHECK(!isValidHttpHeaderValue(std::string_view("a\0b", 3)));
}

RUVIA_TEST(http_method_parsing_is_exact_and_case_sensitive) {
    RUVIA_CHECK(classifyHttpMethod("GET") == HttpKnownMethod::kGet);
    RUVIA_CHECK(classifyHttpMethod("POST") == HttpKnownMethod::kPost);
    RUVIA_CHECK(classifyHttpMethod("PUT") == HttpKnownMethod::kPut);
    RUVIA_CHECK(classifyHttpMethod("DELETE") == HttpKnownMethod::kDelete);
    RUVIA_CHECK(classifyHttpMethod("PATCH") == HttpKnownMethod::kPatch);
    RUVIA_CHECK(classifyHttpMethod("HEAD") == HttpKnownMethod::kHead);
    RUVIA_CHECK(classifyHttpMethod("OPTIONS") == HttpKnownMethod::kOptions);
    RUVIA_CHECK(classifyHttpMethod("CONNECT") == HttpKnownMethod::kConnect);
    // Methods are case-sensitive (RFC 9110 section 9.1); anything outside the
    // framework's fixed semantic set remains an unknown classification.
    RUVIA_CHECK(classifyHttpMethod("get") == HttpKnownMethod::kUnknown);
    RUVIA_CHECK(classifyHttpMethod("Get") == HttpKnownMethod::kUnknown);
    RUVIA_CHECK(classifyHttpMethod("FOO") == HttpKnownMethod::kUnknown);
    RUVIA_CHECK(classifyHttpMethod("") == HttpKnownMethod::kUnknown);
    RUVIA_CHECK(classifyHttpMethod("GETX") == HttpKnownMethod::kUnknown);
}

RUVIA_TEST(http_method_token_validation_is_separate_from_classification) {
    RUVIA_CHECK(isValidHttpMethodToken("PROPFIND"));
    RUVIA_CHECK(isValidHttpMethodToken("get"));
    RUVIA_CHECK(isValidHttpMethodToken("M-SEARCH"));
    RUVIA_CHECK(!isValidHttpMethodToken(""));
    RUVIA_CHECK(!isValidHttpMethodToken("BAD METHOD"));
    RUVIA_CHECK(!isValidHttpMethodToken("BAD(METHOD"));
    RUVIA_CHECK(!isValidHttpMethodToken(std::string_view("BAD\x01METHOD", 10)));
}

RUVIA_TEST(http_known_method_token_round_trips) {
    const HttpKnownMethod methods[] = {
        HttpKnownMethod::kGet, HttpKnownMethod::kPost, HttpKnownMethod::kPut, HttpKnownMethod::kDelete,
        HttpKnownMethod::kPatch, HttpKnownMethod::kHead, HttpKnownMethod::kOptions, HttpKnownMethod::kConnect};
    for (const auto method : methods) {
        RUVIA_CHECK(classifyHttpMethod(knownHttpMethodToken(method)) == method);
    }
    // An unknown classification has no canonical wire spelling. Callers that
    // need it must retain the exact token instead of manufacturing "UNKNOWN".
    RUVIA_CHECK(knownHttpMethodToken(HttpKnownMethod::kUnknown).empty());
}

RUVIA_TEST(http_status_text_validation) {
    // The status reason phrase is validated by the header-value rules, so
    // response-splitting bytes on the status line are rejected.
    RUVIA_CHECK(isValidHttpStatusText("OK"));
    RUVIA_CHECK(isValidHttpStatusText("Not Found"));  // space is allowed
    RUVIA_CHECK(isValidHttpStatusText(""));            // an empty reason phrase is valid
    RUVIA_CHECK(!isValidHttpStatusText(std::string_view("a\r\nb", 4)));
    RUVIA_CHECK(!isValidHttpStatusText(std::string_view("a\nb", 3)));
    RUVIA_CHECK(!isValidHttpStatusText(std::string_view("a\0b", 3)));
}

RUVIA_TEST(http_parse_error_messages) {
    RUVIA_CHECK_EQ(httpParseErrorMessage(HttpParseError::kMissingHost),
                   std::string_view("missing Host header"));
    RUVIA_CHECK_EQ(httpParseErrorMessage(HttpParseError::kHeaderTooLarge),
                   std::string_view("request header is too large"));
    RUVIA_CHECK_EQ(httpParseErrorMessage(HttpParseError::kUnsupportedHttpVersion),
                   std::string_view("unsupported HTTP version"));
    // The two Content-Length faults intentionally share one message.
    RUVIA_CHECK_EQ(httpParseErrorMessage(HttpParseError::kInvalidContentLength),
                   httpParseErrorMessage(HttpParseError::kConflictingContentLength));
    // Reachable errors all map to a non-empty message.
    RUVIA_CHECK(!httpParseErrorMessage(HttpParseError::kChunkSizeOverflow).empty());
    RUVIA_CHECK(!httpParseErrorMessage(HttpParseError::kInvalidTransferEncoding).empty());
    RUVIA_CHECK(!httpParseErrorMessage(HttpParseError::kInvalidConnection).empty());
    RUVIA_CHECK(!httpParseErrorMessage(HttpParseError::kInvalidUpgrade).empty());
}
