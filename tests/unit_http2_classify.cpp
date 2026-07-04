#include "test_harness.h"

#include <string_view>

#include "net/http2/Http2HeaderDecode.h"
#include "net/http2/Http2Upgrade.h"
#include "http/HttpParserInternal.h"
#include "ruvia/http/HttpParseTypes.h"

namespace {

using ruvia::HttpParseError;
using ruvia::detail::HeaderDecodeStatus;
using ruvia::detail::HpackDecodeResult;
using ruvia::detail::HpackError;
using ruvia::detail::http2ClassifyHeaderDecodeResult;
using ruvia::detail::http2ShouldDropInvalidCleartextPreface;
using ruvia::detail::HttpServerParser;
using ruvia::detail::isHttp2UpgradeAttempt;

}  // namespace

RUVIA_TEST(classify_header_decode_result) {
    // A clean decode is OK.
    RUVIA_CHECK(http2ClassifyHeaderDecodeResult(HpackDecodeResult{HpackError::kNone}) ==
                HeaderDecodeStatus::kOk);
    // A header-validation callback rejection is a protocol error.
    RUVIA_CHECK(http2ClassifyHeaderDecodeResult(HpackDecodeResult{HpackError::kCallbackRejected}) ==
                HeaderDecodeStatus::kProtocolError);
    // Any HPACK decoding fault is a compression error (RFC 7541 4.1).
    RUVIA_CHECK(http2ClassifyHeaderDecodeResult(HpackDecodeResult{HpackError::kIntegerOverflow}) ==
                HeaderDecodeStatus::kCompressionError);
    RUVIA_CHECK(http2ClassifyHeaderDecodeResult(HpackDecodeResult{HpackError::kInvalidIndex}) ==
                HeaderDecodeStatus::kCompressionError);
    RUVIA_CHECK(http2ClassifyHeaderDecodeResult(HpackDecodeResult{HpackError::kInvalidHuffman}) ==
                HeaderDecodeStatus::kCompressionError);
}

RUVIA_TEST(drop_invalid_cleartext_only_for_request_line_errors) {
    // Only request-line-shaped parse failures are candidates for a silent drop.
    RUVIA_CHECK(!http2ShouldDropInvalidCleartextPreface("blah blah\r\n", HttpParseError::kNone));
    RUVIA_CHECK(!http2ShouldDropInvalidCleartextPreface("blah blah\r\n", HttpParseError::kMissingHost));
}

RUVIA_TEST(drop_invalid_cleartext_by_version_token) {
    // A request line whose final token is not an HTTP version looks like non-HTTP
    // traffic (a port scan, TLS on a cleartext port) and is dropped.
    RUVIA_CHECK(http2ShouldDropInvalidCleartextPreface("FOO /path GARBAGE\r\n",
                                                       HttpParseError::kInvalidRequestLine));
    RUVIA_CHECK(http2ShouldDropInvalidCleartextPreface("random bytes here\r\n",
                                                       HttpParseError::kUnsupportedMethod));

    // A well-formed HTTP-version token means a real (if unsupported) client -> keep.
    RUVIA_CHECK(!http2ShouldDropInvalidCleartextPreface("XX /p HTTP/1.1\r\n",
                                                        HttpParseError::kUnsupportedMethod));
    RUVIA_CHECK(!http2ShouldDropInvalidCleartextPreface("PRI * HTTP/2.0\r\n",
                                                        HttpParseError::kUnsupportedHttpVersion));

    // No line terminator, or a single token, cannot be classified -> keep.
    RUVIA_CHECK(!http2ShouldDropInvalidCleartextPreface("no-line-break",
                                                        HttpParseError::kInvalidRequestLine));
    RUVIA_CHECK(!http2ShouldDropInvalidCleartextPreface("singletoken\r\n",
                                                        HttpParseError::kInvalidRequestLine));
}

RUVIA_TEST(http2_upgrade_attempt_detection) {
    HttpServerParser parser;
    // An h2c upgrade: Connection: Upgrade plus Upgrade: h2c.
    const auto h2c = parser.parse(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, HTTP2-Settings\r\n"
        "Upgrade: h2c\r\nHTTP2-Settings: AAMAAABkAA\r\n\r\n");
    RUVIA_CHECK(isHttp2UpgradeAttempt(h2c));
    // The token match is case-insensitive.
    const auto upper = parser.parse(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: H2C\r\n\r\n");
    RUVIA_CHECK(isHttp2UpgradeAttempt(upper));

    // A different upgrade protocol is not an h2c attempt.
    const auto websocket = parser.parse(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
    RUVIA_CHECK(!isHttp2UpgradeAttempt(websocket));
    // A plain request is not an upgrade at all.
    const auto plain = parser.parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(!isHttp2UpgradeAttempt(plain));
}
