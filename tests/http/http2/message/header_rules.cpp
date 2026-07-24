#include "test_harness.h"

#include <string_view>

#include "ruvia/http/detail/http2/message/Http2HeaderRules.h"

namespace {

using ruvia::detail::http2HeaderNameHasUppercase;
using ruvia::detail::http2IsForbiddenConnectionHeader;
using ruvia::detail::http2IsForbiddenRequestTrailerHeader;
using ruvia::detail::http2IsForbiddenResponseConnectionField;
using ruvia::detail::http2IsValidDecodedResponseHeader;
using ruvia::detail::http2IsValidRegularHeader;

}  // namespace

RUVIA_TEST(http2_header_name_uppercase_detection) {
    RUVIA_CHECK(http2HeaderNameHasUppercase("Content-Type"));
    RUVIA_CHECK(http2HeaderNameHasUppercase("x-Custom"));
    RUVIA_CHECK(!http2HeaderNameHasUppercase("content-type"));
    RUVIA_CHECK(!http2HeaderNameHasUppercase("x-custom-header"));
    RUVIA_CHECK(!http2HeaderNameHasUppercase(""));
}

RUVIA_TEST(http2_forbidden_connection_headers) {
    // Connection-specific fields must not appear in HTTP/2 (RFC 9113 §8.2.2).
    RUVIA_CHECK(http2IsForbiddenConnectionHeader("connection"));
    RUVIA_CHECK(http2IsForbiddenConnectionHeader("keep-alive"));
    RUVIA_CHECK(http2IsForbiddenConnectionHeader("proxy-connection"));
    RUVIA_CHECK(http2IsForbiddenConnectionHeader("transfer-encoding"));
    RUVIA_CHECK(http2IsForbiddenConnectionHeader("upgrade"));
    RUVIA_CHECK(!http2IsForbiddenConnectionHeader("content-type"));
    // The HTTP/2 check is exact-lowercase; an uppercase form is caught separately.
    RUVIA_CHECK(!http2IsForbiddenConnectionHeader("Connection"));

    // Application response models are version-neutral, so the final-response
    // gate owns a case-insensitive check and forbids TE as well (the trailers
    // exception in RFC 9113 applies only to requests).
    for (const auto name : {"Connection", "keep-alive", "PROXY-CONNECTION", "te", "Transfer-Encoding", "Upgrade"}) {
        RUVIA_CHECK(http2IsForbiddenResponseConnectionField(name));
    }
    RUVIA_CHECK(!http2IsForbiddenResponseConnectionField("content-type"));
}

RUVIA_TEST(http2_valid_regular_header) {
    RUVIA_CHECK(http2IsValidRegularHeader("content-type", "text/html"));
    RUVIA_CHECK(http2IsValidRegularHeader("x-custom", "value"));
    // A pseudo-header or an empty name is not a valid regular header.
    RUVIA_CHECK(!http2IsValidRegularHeader(":path", "/"));
    RUVIA_CHECK(!http2IsValidRegularHeader("", "value"));
    // An uppercase name is malformed.
    RUVIA_CHECK(!http2IsValidRegularHeader("Content-Type", "text/html"));
    // Every connection-specific header is forbidden (RFC 9113 §8.2.2).
    RUVIA_CHECK(!http2IsValidRegularHeader("connection", "close"));
    RUVIA_CHECK(!http2IsValidRegularHeader("keep-alive", "timeout=5"));
    RUVIA_CHECK(!http2IsValidRegularHeader("proxy-connection", "keep-alive"));
    RUVIA_CHECK(!http2IsValidRegularHeader("upgrade", "websocket"));
    RUVIA_CHECK(!http2IsValidRegularHeader("transfer-encoding", "chunked"));
    // TE may carry only "trailers".
    RUVIA_CHECK(http2IsValidRegularHeader("te", "trailers"));
    RUVIA_CHECK(!http2IsValidRegularHeader("te", "gzip"));
    // That exception is request-only; responses cannot carry TE at all.
    RUVIA_CHECK(!http2IsValidDecodedResponseHeader("te", "trailers"));
    RUVIA_CHECK(http2IsValidDecodedResponseHeader("content-type", "text/plain"));
    // A value with CRLF is rejected.
    RUVIA_CHECK(!http2IsValidRegularHeader("x-custom", std::string_view("a\r\nb", 4)));
}

RUVIA_TEST(http2_forbidden_request_trailer_headers) {
    // Fields that govern framing, routing, auth, caching, or state must not appear in
    // an HTTP/2 trailer section -- they are only meaningful in the header block.
    for (const char* name : {"host", "content-length", "connection", "content-encoding", "content-type", "cookie", "authorization", "range", "if-match", "if-none-match", "if-modified-since", "if-unmodified-since", "if-range", "expect", "te", "trailer", "keep-alive", "set-cookie", "max-forwards", "cache-control", "accept-ranges", "content-range", "proxy-authenticate", "proxy-authorization"}) {
        RUVIA_CHECK(http2IsForbiddenRequestTrailerHeader(name));
    }
    // Ordinary content trailers (a checksum, a signature, a trace id) are permitted.
    RUVIA_CHECK(!http2IsForbiddenRequestTrailerHeader("x-checksum"));
    RUVIA_CHECK(!http2IsForbiddenRequestTrailerHeader("accept"));
    RUVIA_CHECK(!http2IsForbiddenRequestTrailerHeader("user-agent"));
}
