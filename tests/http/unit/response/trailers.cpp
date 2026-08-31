#include "test_harness.h"

#include <array>
#include <concepts>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/server/HttpResponseTrailers.h"
#include "ruvia/http/HttpLimits.h"

namespace {

using ruvia::detail::httpResponseTrailerSection;
using ruvia::detail::httpResponseTrailerBlockValid;
using ruvia::detail::HttpResponseTrailerSectionError;
using ruvia::detail::HttpResponseTrailerSectionFailure;
using ruvia::detail::HttpResponseTrailerSectionResult;
using ruvia::detail::isForbiddenResponseTrailerName;
using ruvia::detail::isValidResponseTrailerName;
using ruvia::detail::isValidResponseTrailerValue;
using ruvia::detail::responseTrailerFieldValid;
using ruvia::detail::visitHttpResponseTrailerFields;

template <typename T>
concept HasAnyRvalueTrailerSectionAccessor = requires(T&& result) {
    std::move(result).section();
} || requires(T&& result) { std::move(result).failure(); };

static_assert(!HasAnyRvalueTrailerSectionAccessor<HttpResponseTrailerSectionResult>);
static_assert(std::derived_from<HttpResponseTrailerSectionError, std::exception>);
static_assert(std::is_trivially_copyable_v<HttpResponseTrailerSectionResult>);
static_assert(sizeof(HttpResponseTrailerSectionResult) <= 24);

template <typename T>
concept HasRawTrailerSectionError = requires(const T& failure) { failure.error(); };

static_assert(!HasRawTrailerSectionError<HttpResponseTrailerSectionFailure>);

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
    RUVIA_CHECK(
        !isValidResponseTrailerValue(std::string_view("a\x01"
                                                      "b",
            3)));
    RUVIA_CHECK(
        !isValidResponseTrailerValue(std::string_view("a\x0b"
                                                      "b",
            3)));
    RUVIA_CHECK(
        !isValidResponseTrailerValue(std::string_view("a\x0c"
                                                      "b",
            3)));
    RUVIA_CHECK(
        !isValidResponseTrailerValue(std::string_view("a\x7f"
                                                      "b",
            3)));
}

RUVIA_TEST(response_trailer_forbidden_names) {
    // Framing / connection / routing / content semantics are header-only.
    RUVIA_CHECK(isForbiddenResponseTrailerName("Transfer-Encoding"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Content-Length"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Host"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("TE"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Connection"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Proxy-Connection"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Trailer"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Upgrade"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Content-Type"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Content-Encoding"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Content-Range"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Set-Cookie"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Cache-Control"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Proxy-Authenticate"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Server"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Last-Modified"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Allow"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Access-Control-Allow-Origin"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Access-Control-Allow-Credentials"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Access-Control-Allow-Methods"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Access-Control-Allow-Headers"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Access-Control-Max-Age"));
    RUVIA_CHECK(isForbiddenResponseTrailerName("Access-Control-Expose-Headers"));
    for (const auto name : {"X-Content-Type-Options", "X-Frame-Options",
             "Strict-Transport-Security", "X-XSS-Protection", "Content-Security-Policy",
             "Content-Security-Policy-Report-Only", "Referrer-Policy", "Permissions-Policy",
             "Clear-Site-Data", "WWW-Authenticate", "Content-Disposition"}) {
        RUVIA_CHECK(isForbiddenResponseTrailerName(name));
    }
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
    // RFC 9110 explicitly permits these fields in trailers.
    RUVIA_CHECK(!isForbiddenResponseTrailerName("ETag"));
    RUVIA_CHECK(!isForbiddenResponseTrailerName("Accept-Ranges"));
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
    RUVIA_CHECK(!responseTrailerFieldValid("Proxy-Connection", "keep-alive"));
    RUVIA_CHECK(!responseTrailerFieldValid("Content-Type", "text/plain"));
    RUVIA_CHECK(!responseTrailerFieldValid("Set-Cookie", "a=b"));
    RUVIA_CHECK(!responseTrailerFieldValid("Allow", "GET, POST"));
    RUVIA_CHECK(!responseTrailerFieldValid("Access-Control-Allow-Origin", "*"));
    RUVIA_CHECK(responseTrailerFieldValid("Accept-Ranges", "bytes"));
    // Invalid value.
    RUVIA_CHECK(!responseTrailerFieldValid("X-Trace-Id", std::string_view("a\r\nb", 4)));
    RUVIA_CHECK(!responseTrailerFieldValid("X-Trace-Id", " abc"));
    RUVIA_CHECK(!responseTrailerFieldValid("X-Trace-Id", "abc "));
    RUVIA_CHECK(!responseTrailerFieldValid("X-Trace-Id", "\tabc"));
    RUVIA_CHECK(!responseTrailerFieldValid("X-Trace-Id", "abc\t"));
}

RUVIA_TEST(response_trailer_block_parser_trims_and_uses_response_rules) {
    std::array<ruvia::HttpHeaderView, 2> visited{};
    std::size_t count = 0;
    const auto ok = visitHttpResponseTrailerFields(
        "Accept-Ranges:\tbytes  \r\nServer-Timing: db;dur=4",
        [&](std::string_view name, std::string_view value) {
            visited[count++] = {name, value};
            return true;
        });
    RUVIA_CHECK(ok);
    RUVIA_CHECK_EQ(count, std::size_t{2});
    RUVIA_CHECK_EQ(visited[0].name(), std::string_view("Accept-Ranges"));
    RUVIA_CHECK_EQ(visited[0].value(), std::string_view("bytes"));
    RUVIA_CHECK_EQ(visited[1].name(), std::string_view("Server-Timing"));
    RUVIA_CHECK_EQ(visited[1].value(), std::string_view("db;dur=4"));

    RUVIA_CHECK(httpResponseTrailerBlockValid("ETag: \"abc\"\r\n"));
    RUVIA_CHECK(!httpResponseTrailerBlockValid("Date: Sun, 06 Nov 1994 08:49:37 GMT"));
    RUVIA_CHECK(!httpResponseTrailerBlockValid("Content-Length: 5"));
    RUVIA_CHECK(!httpResponseTrailerBlockValid(" bad: fold"));
    RUVIA_CHECK(!httpResponseTrailerBlockValid(": value"));
}

RUVIA_TEST(response_trailer_section_validation_is_all_fields_or_none) {
    const std::array<ruvia::HttpHeaderView, 2> valid{ruvia::HttpHeaderView{"X-Trace-Id", "abc"},
        ruvia::HttpHeaderView{"Server-Timing", "db;dur=5"}};
    const auto validResult = httpResponseTrailerSection(valid);
    RUVIA_CHECK(validResult.section() != nullptr);
    RUVIA_CHECK(validResult.failure() == nullptr);

    const std::array<ruvia::HttpHeaderView, 2> mixed{
        ruvia::HttpHeaderView{"X-Trace-Id", "abc"}, ruvia::HttpHeaderView{"Content-Length", "5"}};
    const auto mixedResult = httpResponseTrailerSection(mixed);
    RUVIA_CHECK(mixedResult.section() == nullptr);
    RUVIA_CHECK(mixedResult.failure() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(mixedResult.failure()->exception().what()),
        std::string_view("invalid HTTP response trailer section"));
    // An empty field sequence is the valid absence of a trailer section; the
    // submission API reports kEmpty separately when asked to submit one.
    const auto emptyResult = httpResponseTrailerSection({});
    RUVIA_CHECK(emptyResult.section() != nullptr);
}

RUVIA_TEST(response_trailer_section_enforces_field_limits) {
    const std::string oversizedValue(ruvia::kMaxHttpHeaderBytes, 'x');
    const std::array<ruvia::HttpHeaderView, 1> oversized{{
        {"X-Oversized", oversizedValue},
    }};
    const auto oversizedResult = httpResponseTrailerSection(oversized);
    RUVIA_CHECK(oversizedResult.section() == nullptr);
    RUVIA_CHECK(oversizedResult.failure() != nullptr);

    std::array<ruvia::HttpHeaderView, ruvia::kMaxHttpHeaderFields + 1> tooMany{};
    for (auto& header : tooMany) {
        header = {"X-Many", "value"};
    }
    const auto tooManyResult = httpResponseTrailerSection(tooMany);
    RUVIA_CHECK(tooManyResult.section() == nullptr);
    RUVIA_CHECK(tooManyResult.failure() != nullptr);
}
