#include <string>
#include <string_view>

#include "ruvia/http/detail/field/http_trailer_fields.h"

#include "http2/http2_header_rules.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_is_valid_decoded_response_header;
using ruvia::detail::http2_is_valid_regular_header;
using ruvia::detail::is_forbidden_http_binary_connection_field;
using ruvia::detail::is_forbidden_http_binary_response_field;
using ruvia::detail::is_forbidden_http_request_trailer_name;

}  // namespace

RUVIA_TEST(http2_regular_field_name_syntax) {
    RUVIA_CHECK(!http2_is_valid_regular_header("Content-Type", ""));
    RUVIA_CHECK(!http2_is_valid_regular_header("x-Custom", ""));
    RUVIA_CHECK(http2_is_valid_regular_header("content-type", ""));
    RUVIA_CHECK(http2_is_valid_regular_header("x-custom-header", ""));
    RUVIA_CHECK(!http2_is_valid_regular_header("", ""));
}

RUVIA_TEST(http2_regular_field_names_accept_only_lowercase_token_bytes) {
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    for (const std::size_t position : {0U, 4U, 31U, 63U}) {
        std::string name(64, 'x');
        for (unsigned byte = 0; byte < 256; ++byte) {
            name[position] = static_cast<char>(byte);
            const bool allowed = (byte >= 'a' && byte <= 'z') ||
                                 (byte >= '0' && byte <= '9') ||
                                 punctuation.find(static_cast<char>(byte)) != std::string_view::npos;
            RUVIA_CHECK_EQ(http2_is_valid_regular_header(name, "value"), allowed);
            RUVIA_CHECK_EQ(http2_is_valid_decoded_response_header(name, "value"), allowed);
        }
    }
}

RUVIA_TEST(http2_forbidden_connection_headers) {
    // Connection-specific fields must not appear in HTTP/2 (RFC 9113 §8.2.2).
    RUVIA_CHECK(is_forbidden_http_binary_connection_field("connection"));
    RUVIA_CHECK(is_forbidden_http_binary_connection_field("keep-alive"));
    RUVIA_CHECK(is_forbidden_http_binary_connection_field("proxy-connection"));
    RUVIA_CHECK(is_forbidden_http_binary_connection_field("transfer-encoding"));
    RUVIA_CHECK(is_forbidden_http_binary_connection_field("upgrade"));
    RUVIA_CHECK(!is_forbidden_http_binary_connection_field("content-type"));
    // The shared policy accepts application casing; decoded HTTP/2 names are
    // independently required to be lowercase.
    RUVIA_CHECK(is_forbidden_http_binary_connection_field("Connection"));

    // Application response models are version-neutral, so the final-response
    // gate owns a case-insensitive check and forbids TE as well (the trailers
    // exception in RFC 9113 applies only to requests).
    for (const auto name :
        {"Connection", "keep-alive", "PROXY-CONNECTION", "te", "Transfer-Encoding", "Upgrade"}) {
        RUVIA_CHECK(is_forbidden_http_binary_response_field(name));
    }
    RUVIA_CHECK(!is_forbidden_http_binary_response_field("content-type"));
}

RUVIA_TEST(http2_valid_regular_header) {
    RUVIA_CHECK(http2_is_valid_regular_header("content-type", "text/html"));
    RUVIA_CHECK(http2_is_valid_regular_header("x-custom", "value"));
    RUVIA_CHECK(http2_is_valid_regular_header("x-custom", ""));
    // RFC 9113 §8.2.1: HTTP/2 field values cannot start or end with SP/HTAB.
    RUVIA_CHECK(!http2_is_valid_regular_header("x-custom", " value"));
    RUVIA_CHECK(!http2_is_valid_regular_header("x-custom", "value "));
    RUVIA_CHECK(!http2_is_valid_regular_header("x-custom", "\tvalue"));
    RUVIA_CHECK(!http2_is_valid_regular_header("x-custom", "value\t"));
    // A pseudo-header or an empty name is not a valid regular header.
    RUVIA_CHECK(!http2_is_valid_regular_header(":path", "/"));
    RUVIA_CHECK(!http2_is_valid_regular_header("", "value"));
    // An uppercase name is malformed.
    RUVIA_CHECK(!http2_is_valid_regular_header("Content-Type", "text/html"));
    // Every connection-specific header is forbidden (RFC 9113 §8.2.2).
    RUVIA_CHECK(!http2_is_valid_regular_header("connection", "close"));
    RUVIA_CHECK(!http2_is_valid_regular_header("keep-alive", "timeout=5"));
    RUVIA_CHECK(!http2_is_valid_regular_header("proxy-connection", "keep-alive"));
    RUVIA_CHECK(!http2_is_valid_regular_header("upgrade", "websocket"));
    RUVIA_CHECK(!http2_is_valid_regular_header("transfer-encoding", "chunked"));
    // TE may carry only "trailers".
    RUVIA_CHECK(http2_is_valid_regular_header("te", "trailers"));
    RUVIA_CHECK(http2_is_valid_regular_header("te", "Trailers"));
    RUVIA_CHECK(http2_is_valid_regular_header("te", "TRAILERS"));
    RUVIA_CHECK(!http2_is_valid_regular_header("te", "gzip"));
    RUVIA_CHECK(!http2_is_valid_regular_header("te", "trailers, gzip"));
    // That exception is request-only; responses cannot carry TE at all.
    RUVIA_CHECK(!http2_is_valid_decoded_response_header("te", "trailers"));
    RUVIA_CHECK(http2_is_valid_decoded_response_header("content-type", "text/plain"));
    // A value with CRLF is rejected.
    RUVIA_CHECK(!http2_is_valid_regular_header("x-custom", std::string_view("a\r\nb", 4)));
}

RUVIA_TEST(http2_forbidden_request_trailer_headers) {
    // Fields that govern framing, routing, auth, caching, or state must not appear in
    // an HTTP/2 trailer section -- they are only meaningful in the header block.
    for (const char* name : {"host", "content-length", "connection", "content-encoding",
             "content-type", "cookie", "authorization", "range", "if-match", "if-none-match",
             "if-modified-since", "if-unmodified-since", "if-range", "expect", "te", "trailer",
             "keep-alive", "set-cookie", "max-forwards", "cache-control", "accept-ranges",
             "content-range", "proxy-authenticate", "proxy-authorization"}) {
        RUVIA_CHECK(is_forbidden_http_request_trailer_name(name));
    }
    // Ordinary content trailers (a checksum, a signature, a trace id) are permitted.
    RUVIA_CHECK(!is_forbidden_http_request_trailer_name("x-checksum"));
    RUVIA_CHECK(!is_forbidden_http_request_trailer_name("accept"));
    RUVIA_CHECK(!is_forbidden_http_request_trailer_name("user-agent"));
}
