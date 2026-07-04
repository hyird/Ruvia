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
