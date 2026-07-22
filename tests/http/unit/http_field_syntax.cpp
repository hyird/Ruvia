#include "test_harness.h"

#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpParseError.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::HttpParseError;
using ruvia::httpParseProtocolError;
using ruvia::isValidHttpHeaderName;
using ruvia::isValidHttpHeaderValue;
using ruvia::isValidHttpMethodToken;
using ruvia::isValidHttpStatusText;
using ruvia::knownHttpMethodToken;
using ruvia::classifyHttpMethod;

}  // namespace

// What bytes a field name, field value or reason phrase may carry.

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
    const auto missingHost = httpParseProtocolError(HttpParseError::kMissingHost);
    const auto headerTooLarge = httpParseProtocolError(HttpParseError::kHeaderTooLarge);
    const auto unsupportedVersion = httpParseProtocolError(
        HttpParseError::kUnsupportedHttpVersion);
    RUVIA_CHECK_EQ(std::string_view(missingHost.what()),
                   std::string_view("missing Host header"));
    RUVIA_CHECK_EQ(std::string_view(headerTooLarge.what()),
                   std::string_view("request header is too large"));
    RUVIA_CHECK_EQ(std::string_view(unsupportedVersion.what()),
                   std::string_view("unsupported HTTP version"));
    // The two Content-Length faults intentionally share one message.
    const auto invalidLength = httpParseProtocolError(
        HttpParseError::kInvalidContentLength);
    const auto conflictingLength = httpParseProtocolError(
        HttpParseError::kConflictingContentLength);
    RUVIA_CHECK_EQ(std::string_view(invalidLength.what()),
                   std::string_view(conflictingLength.what()));
    // Reachable errors all map to a non-empty message.
    RUVIA_CHECK(std::string_view(httpParseProtocolError(
        HttpParseError::kChunkSizeOverflow).what()).size() != 0);
    RUVIA_CHECK(std::string_view(httpParseProtocolError(
        HttpParseError::kInvalidTransferEncoding).what()).size() != 0);
    RUVIA_CHECK(std::string_view(httpParseProtocolError(
        HttpParseError::kInvalidConnection).what()).size() != 0);
    RUVIA_CHECK(std::string_view(httpParseProtocolError(
        HttpParseError::kInvalidUpgrade).what()).size() != 0);
}
