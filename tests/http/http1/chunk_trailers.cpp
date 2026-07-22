#include "test_harness.h"

#include <cstdint>
#include <string_view>

#include "ruvia/http/HttpParseError.h"
#include "ruvia/http/detail/parser/HttpChunkParser.h"

namespace {

using ruvia::HttpParseError;
using ruvia::httpParseProtocolError;
using ruvia::detail::HttpChunkScanError;
using ruvia::detail::validateHttpChunkTrailers;

}  // namespace

// Which fields a chunked trailer section may carry, and what it must reject.

RUVIA_TEST(chunk_trailers_accept_valid) {
    RUVIA_CHECK(!validateHttpChunkTrailers("").has_value());
    RUVIA_CHECK(!validateHttpChunkTrailers("X-Checksum: abc123\r\n").has_value());
    // A final line without a trailing CRLF is still complete.
    RUVIA_CHECK(!validateHttpChunkTrailers("X-Trace: v").has_value());
}

RUVIA_TEST(chunk_trailers_reject_malformed) {
    // Leading whitespace (obs-fold), missing colon, and an empty name are invalid.
    RUVIA_CHECK(validateHttpChunkTrailers(" X: y\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("no-colon\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers(":value\r\n") == HttpChunkScanError::kInvalidTrailer);
    // A control byte in the value is rejected.
    RUVIA_CHECK(validateHttpChunkTrailers("X: a\x01" "b\r\n") == HttpChunkScanError::kInvalidTrailer);
}

RUVIA_TEST(chunk_trailers_reject_forbidden_fields) {
    // Fields that govern framing/state must not appear in a trailer section.
    RUVIA_CHECK(validateHttpChunkTrailers("TE: trailers\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Trailer: X\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Set-Cookie: a=b\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Content-Encoding: gzip\r\n") == HttpChunkScanError::kInvalidTrailer);
}

RUVIA_TEST(chunk_trailers_reject_framing_and_routing_fields) {
    // The classic trailer-smuggling vectors: message-framing and routing/auth
    // headers injected in the trailer section, which a downstream parser might
    // honor after the head was already processed. These are rejected through the
    // classified-header path (distinct from the name-length switch exercised
    // above), so pin them explicitly -- dropping one reopens trailer smuggling.
    RUVIA_CHECK(validateHttpChunkTrailers("Content-Length: 10\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Transfer-Encoding: chunked\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Host: evil.example\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Connection: close\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Authorization: Bearer x\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Cookie: sid=1\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Origin: https://app.example\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Access-Control-Request-Method: POST\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Access-Control-Request-Headers: X-One\r\n") == HttpChunkScanError::kInvalidTrailer);
    // The classification is case-insensitive, so a lowercase spelling is caught too.
    RUVIA_CHECK(validateHttpChunkTrailers("content-length: 10\r\n") == HttpChunkScanError::kInvalidTrailer);
}

RUVIA_TEST(chunk_trailers_reject_remaining_forbidden_fields) {
    // The forbidden-trailer set has two tiers: the classified-header path and a
    // name-length switch for the less-common names. The tests above cover only a
    // few of each; the rest were unpinned even though the source comment warns
    // that "dropping one reopens trailer smuggling". Cover them all here.
    //
    // Upgrade is a NOTABLE case: the HTTP/1 list forbids it as a trailer, but the
    // HTTP/2 request-trailer set (http2IsForbiddenRequestTrailerHeader) omits it,
    // because HTTP/2 already bans Upgrade as a connection-specific regular header
    // upstream. Over HTTP/1 there is no such upstream ban, so the trailer check is
    // the guard that stops a smuggled protocol-switch request modifier -- pin it.
    RUVIA_CHECK(validateHttpChunkTrailers("Upgrade: websocket\r\n") == HttpChunkScanError::kInvalidTrailer);
    // Proxy-Connection is the fifth connection-specific field, alongside
    // Connection / Keep-Alive / Transfer-Encoding / Upgrade (all pinned here). The
    // HTTP/1 list previously covered four of the five and dropped this one, so a
    // "Proxy-Connection" trailer slipped through even though HTTP/2 rejects it as a
    // connection-specific header -- the same upstream/trailer asymmetry noted for
    // Upgrade above. Pin it so the two protocols agree on the connection set.
    RUVIA_CHECK(validateHttpChunkTrailers("Proxy-Connection: keep-alive\r\n") == HttpChunkScanError::kInvalidTrailer);

    // The name-length switch tier (each an RFC 7230 §4.1.2 / 7231 control or
    // routing/auth field that must not be delivered late in a trailer).
    RUVIA_CHECK(validateHttpChunkTrailers("Keep-Alive: timeout=5\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Max-Forwards: 10\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Cache-Control: no-cache\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Accept-Ranges: bytes\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Content-Range: bytes 0-1/2\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Proxy-Authenticate: Basic\r\n") == HttpChunkScanError::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Proxy-Authorization: Basic eA==\r\n") == HttpChunkScanError::kInvalidTrailer);
    // A genuinely trailer-safe field is still accepted (negative control).
    RUVIA_CHECK(!validateHttpChunkTrailers("X-Checksum: abc\r\n").has_value());
}
