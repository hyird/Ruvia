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

// The status a parse error maps to.

RUVIA_TEST(parse_error_status_mapping) {
    // Size limits map to their specific statuses.
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kHeaderTooLarge).status(), ruvia::http_status::kRequestHeaderFieldsTooLarge);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kTooManyHeaders).status(), ruvia::http_status::kRequestHeaderFieldsTooLarge);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kBodyTooLarge).status(), ruvia::http_status::kContentTooLarge);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kUnsupportedTransferEncoding).status(), ruvia::http_status::kNotImplemented);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kUnsupportedHttpVersion).status(), ruvia::http_status::kHttpVersionNotSupported);
    // Everything else is a 400 Bad Request.
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kMissingHost).status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kInvalidConnection).status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kInvalidUpgrade).status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kInvalidChunkSize).status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kConflictingContentLength).status(), ruvia::http_status::kBadRequest);
}
