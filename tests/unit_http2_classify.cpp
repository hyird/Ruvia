#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "net/http2/Http2HeaderContinuation.h"
#include "net/http2/Http2HeaderDecode.h"
#include "net/http2/Http2Upgrade.h"
#include "HttpParserInternal.h"
#include "ruvia/http/HttpParseTypes.h"

namespace {

using ruvia::HttpParseError;
using ruvia::detail::ConnectionScanner;
using ruvia::detail::http2ReadFramePhase;
using ruvia::detail::HeaderDecodeStatus;
using ruvia::detail::HpackDecodeResult;
using ruvia::detail::HpackError;
using ruvia::detail::http2ClassifyHeaderDecodeResult;
using ruvia::detail::http2ShouldDropInvalidCleartextPreface;
using ruvia::detail::HttpServerParser;
using ruvia::detail::decodeBase64UrlChar;
using ruvia::detail::isHttp2UpgradeAttempt;
using ruvia::detail::parseHttp2UpgradeRequest;

}  // namespace

RUVIA_TEST(http2_read_frame_phase_uses_body_timeout_outside_header_blocks) {
    // A header block still being assembled (awaiting CONTINUATION) keeps the tight
    // header timeout -- the CONTINUATION-flood / slow-loris bound. Every other read
    // wait (request-body DATA, idle between requests) uses the body timeout so a
    // slow legitimate upload is not cut off by the shorter header timeout.
    RUVIA_CHECK(http2ReadFramePhase(/*headerBlockInProgress=*/true) ==
                ConnectionScanner::Phase::kReadingHeader);
    RUVIA_CHECK(http2ReadFramePhase(/*headerBlockInProgress=*/false) ==
                ConnectionScanner::Phase::kReadingBody);
}

RUVIA_TEST(http2_base64url_alphabet_values) {
    // The base64url alphabet (RFC 4648 5): A-Z -> 0-25, a-z -> 26-51,
    // 0-9 -> 52-61, '-' -> 62, '_' -> 63.
    RUVIA_CHECK_EQ(decodeBase64UrlChar('A'), 0);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('Z'), 25);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('a'), 26);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('z'), 51);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('0'), 52);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('9'), 61);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('-'), 62);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('_'), 63);
    // base64url uses '-'/'_', so the standard '+'/'/' and other bytes are invalid.
    RUVIA_CHECK_EQ(decodeBase64UrlChar('+'), -1);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('/'), -1);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('='), -1);
    RUVIA_CHECK_EQ(decodeBase64UrlChar(' '), -1);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('!'), -1);
}

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

RUVIA_TEST(parse_http2_upgrade_request) {
    HttpServerParser parser;
    auto* const resource = std::pmr::new_delete_resource();

    // A valid h2c upgrade with a well-formed HTTP2-Settings (6-byte entry).
    const auto valid = parser.parse(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, HTTP2-Settings\r\n"
        "Upgrade: h2c\r\nHTTP2-Settings: AAQAAQAA\r\n\r\n");
    const auto validResult = parseHttp2UpgradeRequest(valid, resource);
    RUVIA_CHECK(validResult.valid);
    RUVIA_CHECK_EQ(validResult.settingsPayload.size(), std::size_t{6});

    // RFC field-line combination means split Connection fields still carry the
    // same token set; the upgrade path must not depend on the cached last value.
    const auto splitConnection = parser.parse(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: HTTP2-Settings\r\n"
        "Connection: Upgrade\r\nUpgrade: h2c\r\nHTTP2-Settings: AAQAAQAA\r\n\r\n");
    RUVIA_CHECK(parseHttp2UpgradeRequest(splitConnection, resource).valid);

    // Not an h2c upgrade -> invalid.
    const auto websocket = parser.parse(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
    RUVIA_CHECK(!parseHttp2UpgradeRequest(websocket, resource).valid);

    // h2c, but Connection lacks the HTTP2-Settings token -> invalid.
    const auto noToken = parser.parse(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: h2c\r\n"
        "HTTP2-Settings: AAQAAQAA\r\n\r\n");
    RUVIA_CHECK(!parseHttp2UpgradeRequest(noToken, resource).valid);

    // Duplicate HTTP2-Settings headers -> invalid.
    const auto duplicate = parser.parse(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, HTTP2-Settings\r\n"
        "Upgrade: h2c\r\nHTTP2-Settings: AAQAAQAA\r\nHTTP2-Settings: AAQAAQAA\r\n\r\n");
    RUVIA_CHECK(!parseHttp2UpgradeRequest(duplicate, resource).valid);

    // A duplicate whose first occurrence is empty is still a duplicate (the
    // second must not be accepted just because the first carried no value).
    const auto emptyFirstDuplicate = parser.parse(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, HTTP2-Settings\r\n"
        "Upgrade: h2c\r\nHTTP2-Settings: \r\nHTTP2-Settings: AAQAAQAA\r\n\r\n");
    RUVIA_CHECK(!parseHttp2UpgradeRequest(emptyFirstDuplicate, resource).valid);

    // The Connection token advertises HTTP2-Settings but the header is absent
    // -> malformed (exactly one HTTP2-Settings header is required).
    const auto missingHeader = parser.parse(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, HTTP2-Settings\r\n"
        "Upgrade: h2c\r\n\r\n");
    RUVIA_CHECK(!parseHttp2UpgradeRequest(missingHeader, resource).valid);

    // Malformed base64url in HTTP2-Settings -> invalid.
    const auto badSettings = parser.parse(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, HTTP2-Settings\r\n"
        "Upgrade: h2c\r\nHTTP2-Settings: @@@bad\r\n\r\n");
    RUVIA_CHECK(!parseHttp2UpgradeRequest(badSettings, resource).valid);
}
