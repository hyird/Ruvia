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
}

RUVIA_TEST(response_trailer_forbidden_names) {
    // Framing / connection / routing fields are forbidden in a trailer section.
    RUVIA_CHECK(isForbiddenResponseTrailerName("Transfer-Encoding"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Content-Length"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Host"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("TE"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Connection"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Trailer"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Upgrade"));
    // The check is case-insensitive.
    RUVIA_CHECK(isForbiddenResponseTrailerName("transfer-encoding"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("content-length"));
    // An ordinary field is allowed.
    RUVIA_CHECK(!isForbiddenResponseTrailerName("X-Trace-Id"));
}

RUVIA_TEST(response_trailer_field_combined_rule) {
    RUVIA_CHECK(responseTrailerFieldValid("X-Trace-Id", "abc123"));
    RUVIA_CHECK(responseTrailerFieldValid("Server-Timing", "db;dur=53"));
    // Invalid name.
    RUVIA_CHECK(!responseTrailerFieldValid(":status", "200"));
    // Forbidden name.
    RUVIA_CHECK(!responseTrailerFieldValid("Content-Length", "5"));
    RUVIA_CHECK(!responseTrailerFieldValid("transfer-encoding", "chunked"));
    // Invalid value.
    RUVIA_CHECK(!responseTrailerFieldValid("X-Trace-Id", std::string_view("a\r\nb", 4)));
}
