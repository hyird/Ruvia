#include "test_harness.h"

#include <string_view>

#include "http/HttpParserInternal.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpParseTypes.h"
#include "ruvia/http/HttpRequest.h"

namespace {

using ruvia::HttpMethod;
using ruvia::HttpParseError;
using ruvia::HttpParseStatus;
using ruvia::detail::HttpServerParser;

}  // namespace

RUVIA_TEST(http1_parse_valid_request) {
    HttpServerParser parser;
    const auto result = parser.parse("GET /path?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kComplete);
    RUVIA_CHECK(result.request.method() == HttpMethod::kGet);
    RUVIA_CHECK_EQ(result.request.path(), std::string_view("/path"));
    RUVIA_CHECK_EQ(result.request.queryString(), std::string_view("q=1"));
    RUVIA_CHECK_EQ(result.request.header("host"), std::string_view("example.com"));
}

RUVIA_TEST(http1_parse_incomplete_head) {
    HttpServerParser parser;
    // No terminating blank line yet -> incomplete, keep reading.
    const auto result = parser.parse("GET / HTTP/1.1\r\nHost: example.com\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kIncomplete);
}

RUVIA_TEST(http1_parse_missing_host_rejected) {
    HttpServerParser parser;
    const auto result = parser.parse("GET / HTTP/1.1\r\n\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kError);
    RUVIA_CHECK(result.error == HttpParseError::kMissingHost);
}

RUVIA_TEST(http1_parse_invalid_request_line_rejected) {
    HttpServerParser parser;
    const auto result = parser.parse("!!!garbage!!!\r\n\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kError);
}

RUVIA_TEST(http1_parse_content_length_body) {
    HttpServerParser parser;
    const auto result = parser.parse(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
    RUVIA_CHECK(result.status == HttpParseStatus::kComplete);
    RUVIA_CHECK(result.request.method() == HttpMethod::kPost);
    RUVIA_CHECK_EQ(result.contentLength, std::size_t{5});
}

RUVIA_TEST(http1_parse_conflicting_content_length_rejected) {
    HttpServerParser parser;
    const auto result = parser.parse(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kError);
    RUVIA_CHECK(result.error == HttpParseError::kConflictingContentLength);
}

RUVIA_TEST(http1_parse_content_length_with_transfer_encoding_rejected) {
    // TE + CL together is a request-smuggling vector and must be rejected
    // (RFC 7230 3.3.3).
    HttpServerParser parser;
    const auto result = parser.parse(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kError);
}

RUVIA_TEST(http1_parse_chunked_body) {
    HttpServerParser parser;
    const auto result = parser.parse(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kComplete);
    RUVIA_CHECK(result.chunked);
}

RUVIA_TEST(http1_parse_unsupported_version_rejected) {
    HttpServerParser parser;
    const auto result = parser.parse("GET / HTTP/2.0\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kError);
    RUVIA_CHECK(result.error == HttpParseError::kUnsupportedHttpVersion);
}

RUVIA_TEST(http1_parse_unsupported_method_rejected) {
    HttpServerParser parser;
    const auto result = parser.parse("FOOBAR / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kError);
    RUVIA_CHECK(result.error == HttpParseError::kUnsupportedMethod);
}

RUVIA_TEST(http1_parse_transfer_encoding_not_chunked_rejected) {
    // A non-chunked Transfer-Encoding leaves message framing undetermined.
    HttpServerParser parser;
    const auto result = parser.parse(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kError);
    RUVIA_CHECK(result.error == HttpParseError::kInvalidTransferEncoding);
}

RUVIA_TEST(http1_parse_transfer_encoding_in_http10_rejected) {
    // Transfer-Encoding in HTTP/1.0 is faulty framing (RFC 9112 6.1).
    HttpServerParser parser;
    const auto result = parser.parse(
        "POST / HTTP/1.0\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kError);
    RUVIA_CHECK(result.error == HttpParseError::kInvalidTransferEncoding);
}

RUVIA_TEST(http1_parse_absolute_uri_host_mismatch_rejected) {
    // An absolute-form target whose authority disagrees with the Host header is
    // rejected to prevent routing ambiguity.
    HttpServerParser parser;
    const auto result = parser.parse(
        "GET http://a.example/ HTTP/1.1\r\nHost: b.example\r\n\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kError);
    RUVIA_CHECK(result.error == HttpParseError::kInvalidHost);
}

RUVIA_TEST(http1_parse_http10_without_host_allowed) {
    // HTTP/1.0 does not require a Host header.
    HttpServerParser parser;
    const auto result = parser.parse("GET / HTTP/1.0\r\n\r\n");
    RUVIA_CHECK(result.status == HttpParseStatus::kComplete);
    RUVIA_CHECK(result.request.method() == HttpMethod::kGet);
}
