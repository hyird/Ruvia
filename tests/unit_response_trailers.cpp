#include "test_harness.h"

#include <string_view>

#include "net/server/HttpResponseTrailers.h"

namespace {

using ruvia::detail::isForbiddenResponseTrailerName;
using ruvia::detail::isValidResponseTrailerName;
using ruvia::detail::isValidResponseTrailerValue;
using ruvia::detail::responseTrailerFieldValid;

}  // namespace

RUVIA_TEST(response_trailer_name_is_a_token) {
    RUVIA_CHECK(isValidResponseTrailerName("X-Trace-Id"));
    RUVIA_CHECK(isValidResponseTrailerName("ETag"));
    // Empty, whitespace, and control bytes are not tokens.
    RUVIA_CHECK(!isValidResponseTrailerName(""));
    RUVIA_CHECK(!isValidResponseTrailerName("bad name"));
    RUVIA_CHECK(!isValidResponseTrailerName(std::string_view("x\x01y", 3)));
    // A colon is not a tchar, so pseudo-headers can never pass (RFC 9113 8.1).
    RUVIA_CHECK(!isValidResponseTrailerName(":status"));
}

RUVIA_TEST(response_trailer_value_rejects_splitting_bytes) {
    RUVIA_CHECK(isValidResponseTrailerValue("plain-value"));
    RUVIA_CHECK(isValidResponseTrailerValue(""));
    // obs-text (high bytes) is permitted.
    RUVIA_CHECK(isValidResponseTrailerValue(std::string_view("v\x80z", 3)));
    // CR, LF, and NUL are forbidden — this is what stops response splitting.
    RUVIA_CHECK(!isValidResponseTrailerValue(std::string_view("a\rb", 3)));
    RUVIA_CHECK(!isValidResponseTrailerValue(std::string_view("a\nb", 3)));
    RUVIA_CHECK(!isValidResponseTrailerValue(std::string_view("a\r\nb", 4)));
    RUVIA_CHECK(!isValidResponseTrailerValue(std::string_view("a\0b", 3)));
    // HTAB and SP are field-value bytes and stay allowed.
    RUVIA_CHECK(isValidResponseTrailerValue(std::string_view("a\tb c", 5)));
    // The other control bytes are not field-vchar either (RFC 9110 §5.5), so a
    // non-splitting control like 0x01, VT (0x0B), FF (0x0C), or DEL (0x7F) must
    // also be rejected -- matching the request-trailer and header-value checks.
    RUVIA_CHECK(!isValidResponseTrailerValue(std::string_view("a\x01" "b", 3)));
    RUVIA_CHECK(!isValidResponseTrailerValue(std::string_view("a\x0b" "b", 3)));
    RUVIA_CHECK(!isValidResponseTrailerValue(std::string_view("a\x0c" "b", 3)));
    RUVIA_CHECK(!isValidResponseTrailerValue(std::string_view("a\x7f" "b", 3)));
}

RUVIA_TEST(response_trailer_forbidden_names) {
    // Framing / connection / routing / content semantics are header-only.
    RUVIA_CHECK(isForbiddenResponseTrailerName("Transfer-Encoding"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Content-Length"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Host"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("TE"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Connection"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Trailer"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Upgrade"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Content-Type"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Content-Encoding"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Content-Range"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Set-Cookie"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Cache-Control"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Proxy-Authenticate"));
    // Response control data (RFC 9110 §6.5.1) must be processed before the content
    // and thus cannot be trailered: a recipient may discard trailers, silently
    // dropping the redirect/cache/auth-timing control.
    RUVIA_CHECK(isForbiddenResponseTrailerName("Age"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Date"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Vary"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Pragma"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Expires"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Warning"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Location"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Retry-After"));
    // The check is case-insensitive.
    RUVIA_CHECK(isForbiddenResponseTrailerName("transfer-encoding"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("content-length"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("content-type"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("location"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("retry-after"));
    // An ordinary field is allowed.
    RUVIA_CHECK(!isForbiddenResponseTrailerName("X-Trace-Id"));
    // Fields that are legitimately computed after the body stay allowed.
    RUVIA_CHECK(!isForbiddenResponseTrailerName("ETag"));
    RUVIA_CHECK(!isForbiddenResponseTrailerName("Server-Timing"));
}

RUVIA_TEST(response_trailer_field_combined_rule) {
    RUVIA_CHECK(responseTrailerFieldValid("X-Trace-Id", "abc123"));
    RUVIA_CHECK(responseTrailerFieldValid("Server-Timing", "db;dur=53"));
    // Invalid name.
    RUVIA_CHECK(!responseTrailerFieldValid(":status", "200"));
    // Forbidden name.
    RUVIA_CHECK(!responseTrailerFieldValid("Content-Length", "5"));
    RUVIA_CHECK(!responseTrailerFieldValid("transfer-encoding", "chunked"));
    RUVIA_CHECK(!responseTrailerFieldValid("Content-Type", "text/plain"));
    RUVIA_CHECK(!responseTrailerFieldValid("Set-Cookie", "a=b"));
    // Invalid value.
    RUVIA_CHECK(!responseTrailerFieldValid("X-Trace-Id", std::string_view("a\r\nb", 4)));
}
