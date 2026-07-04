#include "test_harness.h"

#include <cstdint>
#include <string_view>

#include "ruvia/http/HttpParseTypes.h"
#include "http/parser/HttpChunkParser.h"

namespace {

using ruvia::HttpParseError;
using ruvia::httpParseErrorStatus;
using ruvia::detail::HttpChunkScanStatus;
using ruvia::detail::validateHttpChunkTrailers;

}  // namespace

RUVIA_TEST(parse_error_status_mapping) {
    // Size limits map to their specific statuses.
    RUVIA_CHECK_EQ(httpParseErrorStatus(HttpParseError::kHeaderTooLarge), std::uint16_t{431});
    RUVIA_CHECK_EQ(httpParseErrorStatus(HttpParseError::kTooManyHeaders), std::uint16_t{431});
    RUVIA_CHECK_EQ(httpParseErrorStatus(HttpParseError::kBodyTooLarge), std::uint16_t{413});
    RUVIA_CHECK_EQ(httpParseErrorStatus(HttpParseError::kUnsupportedMethod), std::uint16_t{501});
    RUVIA_CHECK_EQ(httpParseErrorStatus(HttpParseError::kUnsupportedHttpVersion), std::uint16_t{505});
    RUVIA_CHECK_EQ(httpParseErrorStatus(HttpParseError::kExpectationFailed), std::uint16_t{417});
    // Everything else is a 400 Bad Request.
    RUVIA_CHECK_EQ(httpParseErrorStatus(HttpParseError::kMissingHost), std::uint16_t{400});
    RUVIA_CHECK_EQ(httpParseErrorStatus(HttpParseError::kInvalidChunkSize), std::uint16_t{400});
    RUVIA_CHECK_EQ(httpParseErrorStatus(HttpParseError::kConflictingContentLength), std::uint16_t{400});
}

RUVIA_TEST(chunk_trailers_accept_valid) {
    RUVIA_CHECK(validateHttpChunkTrailers("") == HttpChunkScanStatus::kComplete);
    RUVIA_CHECK(validateHttpChunkTrailers("X-Checksum: abc123\r\n") == HttpChunkScanStatus::kComplete);
    // A final line without a trailing CRLF is still complete.
    RUVIA_CHECK(validateHttpChunkTrailers("X-Trace: v") == HttpChunkScanStatus::kComplete);
}

RUVIA_TEST(chunk_trailers_reject_malformed) {
    // Leading whitespace (obs-fold), missing colon, and an empty name are invalid.
    RUVIA_CHECK(validateHttpChunkTrailers(" X: y\r\n") == HttpChunkScanStatus::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("no-colon\r\n") == HttpChunkScanStatus::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers(":value\r\n") == HttpChunkScanStatus::kInvalidTrailer);
    // A control byte in the value is rejected.
    RUVIA_CHECK(validateHttpChunkTrailers("X: a\x01" "b\r\n") == HttpChunkScanStatus::kInvalidTrailer);
}

RUVIA_TEST(chunk_trailers_reject_forbidden_fields) {
    // Fields that govern framing/state must not appear in a trailer section.
    RUVIA_CHECK(validateHttpChunkTrailers("TE: trailers\r\n") == HttpChunkScanStatus::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Trailer: X\r\n") == HttpChunkScanStatus::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Set-Cookie: a=b\r\n") == HttpChunkScanStatus::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Content-Encoding: gzip\r\n") == HttpChunkScanStatus::kInvalidTrailer);
}

RUVIA_TEST(chunk_trailers_reject_framing_and_routing_fields) {
    // The classic trailer-smuggling vectors: message-framing and routing/auth
    // headers injected in the trailer section, which a downstream parser might
    // honor after the head was already processed. These are rejected through the
    // classified-header path (distinct from the name-length switch exercised
    // above), so pin them explicitly -- dropping one reopens trailer smuggling.
    RUVIA_CHECK(validateHttpChunkTrailers("Content-Length: 10\r\n") == HttpChunkScanStatus::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Transfer-Encoding: chunked\r\n") == HttpChunkScanStatus::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Host: evil.example\r\n") == HttpChunkScanStatus::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Connection: close\r\n") == HttpChunkScanStatus::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Authorization: Bearer x\r\n") == HttpChunkScanStatus::kInvalidTrailer);
    RUVIA_CHECK(validateHttpChunkTrailers("Cookie: sid=1\r\n") == HttpChunkScanStatus::kInvalidTrailer);
    // The classification is case-insensitive, so a lowercase spelling is caught too.
    RUVIA_CHECK(validateHttpChunkTrailers("content-length: 10\r\n") == HttpChunkScanStatus::kInvalidTrailer);
}
