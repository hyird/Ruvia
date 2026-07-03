#include "test_harness.h"

#include <string_view>

#include "net/http2/Http2HeaderRules.h"

namespace {

using ruvia::detail::http2HeaderNameHasUppercase;
using ruvia::detail::http2IsForbiddenConnectionHeader;
using ruvia::detail::http2IsForbiddenUpgradedRequestHeader;
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
    // Connection-specific fields must not appear in HTTP/2 (RFC 7540 8.1.2.2).
    RUVIA_CHECK(http2IsForbiddenConnectionHeader("connection"));
    RUVIA_CHECK(http2IsForbiddenConnectionHeader("keep-alive"));
    RUVIA_CHECK(http2IsForbiddenConnectionHeader("proxy-connection"));
    RUVIA_CHECK(http2IsForbiddenConnectionHeader("transfer-encoding"));
    RUVIA_CHECK(http2IsForbiddenConnectionHeader("upgrade"));
    RUVIA_CHECK(!http2IsForbiddenConnectionHeader("content-type"));
    // The HTTP/2 check is exact-lowercase; an uppercase form is caught separately.
    RUVIA_CHECK(!http2IsForbiddenConnectionHeader("Connection"));
}

RUVIA_TEST(http2_forbidden_upgraded_request_headers) {
    // The h2c-upgrade path strips these case-insensitively.
    RUVIA_CHECK(http2IsForbiddenUpgradedRequestHeader("Connection"));
    RUVIA_CHECK(http2IsForbiddenUpgradedRequestHeader("upgrade"));
    RUVIA_CHECK(http2IsForbiddenUpgradedRequestHeader("HTTP2-Settings"));
    RUVIA_CHECK(http2IsForbiddenUpgradedRequestHeader("Keep-Alive"));
    RUVIA_CHECK(http2IsForbiddenUpgradedRequestHeader("TRANSFER-ENCODING"));
    RUVIA_CHECK(!http2IsForbiddenUpgradedRequestHeader("content-type"));
}

RUVIA_TEST(http2_valid_regular_header) {
    RUVIA_CHECK(http2IsValidRegularHeader("content-type", "text/html"));
    RUVIA_CHECK(http2IsValidRegularHeader("x-custom", "value"));
    // A pseudo-header or an empty name is not a valid regular header.
    RUVIA_CHECK(!http2IsValidRegularHeader(":path", "/"));
    RUVIA_CHECK(!http2IsValidRegularHeader("", "value"));
    // An uppercase name is malformed.
    RUVIA_CHECK(!http2IsValidRegularHeader("Content-Type", "text/html"));
    // Connection-specific headers are forbidden.
    RUVIA_CHECK(!http2IsValidRegularHeader("connection", "close"));
    RUVIA_CHECK(!http2IsValidRegularHeader("transfer-encoding", "chunked"));
    // TE may carry only "trailers".
    RUVIA_CHECK(http2IsValidRegularHeader("te", "trailers"));
    RUVIA_CHECK(!http2IsValidRegularHeader("te", "gzip"));
    // A value with CRLF is rejected.
    RUVIA_CHECK(!http2IsValidRegularHeader("x-custom", std::string_view("a\r\nb", 4)));
}
