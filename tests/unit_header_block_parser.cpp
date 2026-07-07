#include "test_harness.h"

#include <cstddef>
#include <string_view>

#include "parser/HttpHeaderBlockParser.h"
#include "ruvia/http/HttpParseTypes.h"

namespace {

using ruvia::HttpParseError;
using ruvia::detail::findHttpHeaderEnd;
using ruvia::detail::ParsedRequestHeaderBlock;
using ruvia::detail::parseHttpHeaderBlock;

struct Parsed final {
    HttpParseError error;
    bool hasHost;
    bool sawChunked;
    bool sawContentLength;
    std::size_t contentLength;
    std::size_t transferCodingCount;
    ruvia::detail::HttpTransferCoding firstTransferCoding;
};

Parsed parse(std::string_view head) {
    ParsedRequestHeaderBlock block{};
    const auto headerBytes = findHttpHeaderEnd(head, 0);
    const auto error = parseHttpHeaderBlock(head, headerBytes, block);
    return {error, block.flags.hasHost, block.sawChunked, block.sawContentLength, block.contentLength,
            block.transferCodings.count, block.transferCodings.values[0]};
}

}  // namespace

RUVIA_TEST(header_block_parses_valid_request) {
    const auto result = parse("GET / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kNone);
    RUVIA_CHECK(result.hasHost);
    RUVIA_CHECK(result.sawContentLength);
    RUVIA_CHECK_EQ(result.contentLength, std::size_t{5});
}

RUVIA_TEST(header_block_rejects_conflicting_content_length) {
    // Two Content-Length values that disagree is a classic request-smuggling
    // vector and must be rejected.
    const auto result = parse(
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kConflictingContentLength);
}

RUVIA_TEST(header_block_allows_repeated_equal_content_length) {
    const auto result = parse(
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kNone);
    RUVIA_CHECK_EQ(result.contentLength, std::size_t{5});
}

RUVIA_TEST(header_block_rejects_invalid_content_length) {
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n").error ==
                HttpParseError::kInvalidContentLength);
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: -5\r\n\r\n").error ==
                HttpParseError::kInvalidContentLength);
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5x\r\n\r\n").error ==
                HttpParseError::kInvalidContentLength);
}

RUVIA_TEST(header_block_rejects_duplicate_host) {
    // A duplicate Host header is ambiguous (host confusion) and must be rejected.
    const auto result = parse("GET / HTTP/1.1\r\nHost: a.com\r\nHost: b.com\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kInvalidHost);
}

RUVIA_TEST(header_block_rejects_duplicate_content_type) {
    // Content-Type is a singleton representation header; accepting duplicates
    // lets body helpers observe only the cached last value.
    const auto result = parse(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Type: application/json\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kInvalidHeader);
}

RUVIA_TEST(header_block_rejects_duplicate_range) {
    // Ruvia file responses only support a single byte-range set. Repeated Range
    // fields must not degrade to cached last-value behavior.
    const auto result = parse(
        "GET /file HTTP/1.1\r\nHost: x\r\n"
        "Range: bytes=0-99\r\n"
        "Range: bytes=200-299\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kInvalidHeader);
}

RUVIA_TEST(header_block_rejects_duplicate_conditional_header) {
    // Conditional headers drive 304/412 file responses. Repeated fields must
    // not degrade to known-header cached last-value behavior.
    const auto result = parse(
        "GET /file HTTP/1.1\r\nHost: x\r\n"
        "If-None-Match: \"old\"\r\n"
        "If-None-Match: \"new\"\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kInvalidHeader);
}

RUVIA_TEST(header_block_rejects_duplicate_auth_and_cors_singletons) {
    RUVIA_CHECK(parse(
                    "GET / HTTP/1.1\r\nHost: x\r\n"
                    "Authorization: Bearer first\r\n"
                    "Authorization: Bearer second\r\n\r\n")
                    .error == HttpParseError::kInvalidHeader);
    RUVIA_CHECK(parse(
                    "GET / HTTP/1.1\r\nHost: x\r\n"
                    "Origin: https://a.example\r\n"
                    "Origin: https://b.example\r\n\r\n")
                    .error == HttpParseError::kInvalidHeader);
    RUVIA_CHECK(parse(
                    "OPTIONS / HTTP/1.1\r\nHost: x\r\n"
                    "Access-Control-Request-Method: GET\r\n"
                    "Access-Control-Request-Method: POST\r\n\r\n")
                    .error == HttpParseError::kInvalidHeader);
}

RUVIA_TEST(header_block_rejects_invalid_bracketed_host_literal) {
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: [::1]\r\n\r\n").error == HttpParseError::kNone);
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: [::::]\r\n\r\n").error == HttpParseError::kInvalidHost);
}

RUVIA_TEST(header_block_accepts_transfer_encoding_chunked) {
    const auto result = parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kNone);
    RUVIA_CHECK(result.sawChunked);
}

RUVIA_TEST(header_block_accepts_transfer_encoding_quoted_comma_parameter) {
    const auto result = parse(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked;note=\"a,b\"\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kNone);
    RUVIA_CHECK(result.sawChunked);
}

RUVIA_TEST(header_block_transfer_coding_combined_with_chunked_is_rejected) {
    // Despite the HttpTransferCoding.h comment ("one request transfer-coding is
    // allowed before final chunked framing"), the parser REJECTS a coding combined
    // with chunked: "gzip, chunked" is unsupported, only bare "chunked" is accepted.
    // Pin the real, conservative behavior (a coding+chunked would require staging a
    // decoded body, which this framing deliberately avoids).
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n").error ==
                HttpParseError::kUnsupportedTransferEncoding);

    // At the header-block layer a lone non-chunked coding parses and IS recorded in
    // transferCodings (the population path). It is the request-level parser that
    // then rejects any Transfer-Encoding whose final coding isn't chunked, so a
    // non-empty transferCodings never actually survives to an accepted request.
    const auto lone = parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n");
    RUVIA_CHECK(lone.error == HttpParseError::kNone);
    RUVIA_CHECK(!lone.sawChunked);
    RUVIA_CHECK_EQ(lone.transferCodingCount, std::size_t{1});
    RUVIA_CHECK(lone.firstTransferCoding == ruvia::detail::HttpTransferCoding::kGzip);
}

RUVIA_TEST(header_block_rejects_smuggling_transfer_encodings) {
    // "chunked" must be the final coding.
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, gzip\r\n\r\n").error ==
                HttpParseError::kInvalidTransferEncoding);
    // A second coding after chunked (here a repeated header) is malformed.
    RUVIA_CHECK(parse(
                    "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n")
                    .error == HttpParseError::kInvalidTransferEncoding);
    // An unknown coding is unsupported.
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: bogus\r\n\r\n").error ==
                HttpParseError::kUnsupportedTransferEncoding);
    // More than one non-chunked coding exceeds the single-coding limit.
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, deflate, chunked\r\n\r\n").error ==
                HttpParseError::kUnsupportedTransferEncoding);
    // Empty transfer-coding list items are malformed in this framing-sensitive header.
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ,chunked\r\n\r\n").error ==
                HttpParseError::kInvalidTransferEncoding);
}

RUVIA_TEST(header_block_content_length_edge_cases) {
    // OWS around the value is trimmed.
    const auto ows = parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length:   42  \r\n\r\n");
    RUVIA_CHECK(ows.error == HttpParseError::kNone);
    RUVIA_CHECK_EQ(ows.contentLength, std::size_t{42});
    // Leading zeros parse to the same numeric value (no desync).
    const auto zeros = parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 007\r\n\r\n");
    RUVIA_CHECK(zeros.error == HttpParseError::kNone);
    RUVIA_CHECK_EQ(zeros.contentLength, std::size_t{7});
    // A '+' sign, a hex form, and overflow are all rejected rather than wrapped.
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\n").error ==
                HttpParseError::kInvalidContentLength);
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0x10\r\n\r\n").error ==
                HttpParseError::kInvalidContentLength);
    RUVIA_CHECK(parse(
                    "GET / HTTP/1.1\r\nHost: x\r\n"
                    "Content-Length: 99999999999999999999999999\r\n\r\n")
                    .error == HttpParseError::kInvalidContentLength);
}
