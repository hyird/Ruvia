#include <array>
#include <concepts>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"

#include "test_harness.h"

namespace {

using ruvia::detail::httpResponseTrailerBlockValid;
using ruvia::detail::httpResponseTrailerSection;
using ruvia::detail::HttpResponseTrailerSectionError;
using ruvia::detail::HttpResponseTrailerSectionFailure;
using ruvia::detail::HttpResponseTrailerSectionResult;
using ruvia::detail::isForbiddenResponseTrailerName;
using ruvia::detail::responseTrailerFieldValid;
using ruvia::detail::visitHttpResponseTrailerFields;

}  // namespace

RUVIA_TEST(response_trailer_name_is_a_token) {
    RUVIA_CHECK(responseTrailerFieldValid("X-Trace-Id", ""));
    RUVIA_CHECK(responseTrailerFieldValid("ETag", ""));
    // Empty, whitespace, and control bytes are not tokens.
    RUVIA_CHECK(!responseTrailerFieldValid("", ""));
    RUVIA_CHECK(!responseTrailerFieldValid("bad name", ""));
    RUVIA_CHECK(!responseTrailerFieldValid(std::string_view("x\x01y", 3), ""));
    // A colon is not a tchar, so pseudo-headers can never pass (RFC 9113 8.1).
    RUVIA_CHECK(!responseTrailerFieldValid(":status", ""));
}

RUVIA_TEST(response_trailer_value_rejects_splitting_bytes) {
    for (const auto value : {std::string_view("plain-value"), std::string_view{},
             std::string_view("v\x80z", 3), std::string_view("a\tb c", 5)}) {
        RUVIA_CHECK(responseTrailerFieldValid("x-trace", value));
    }
    // CR, LF, NUL, other controls and DEL are forbidden; internal HTAB,
    // SP and obs-text above retain the common field-value contract.
    for (const auto value : {std::string_view("a\rb", 3), std::string_view("a\nb", 3),
             std::string_view("a\r\nb", 4), std::string_view("a\0b", 3),
             std::string_view("a\x01"
                              "b",
                 3),
             std::string_view("a\x0b"
                              "b",
                 3),
             std::string_view("a\x0c"
                              "b",
                 3),
             std::string_view("a\x7f"
                              "b",
                 3)}) {
        RUVIA_CHECK(!responseTrailerFieldValid("x-trace", value));
    }
}

RUVIA_TEST(response_trailer_values_validate_short_and_long_field_bytes) {
    for (const std::size_t length : {1U, 2U, 3U, 31U, 32U, 33U, 34U, 35U, 64U, 4096U}) {
        for (const auto position : {std::size_t{0}, length / 2, length - 1}) {
            std::string value(length, 'x');
            for (unsigned byte = 0; byte < 256; ++byte) {
                value[position] = static_cast<char>(byte);
                const bool whitespace = byte == '\t' || byte == ' ';
                const bool allowed_byte = byte == '\t' || (byte >= 32 && byte != 127);
                const bool allowed = allowed_byte &&
                                     (!(position == 0 || position + 1 == length) || !whitespace);
                RUVIA_CHECK_EQ(ruvia::isValidHttpHeaderValue(value), allowed);
                RUVIA_CHECK_EQ(responseTrailerFieldValid("x-checksum", value), allowed);
                const std::array fields{ruvia::HttpHeaderView("x-checksum", value)};
                const auto section = httpResponseTrailerSection(fields);
                RUVIA_CHECK_EQ(section.section() != nullptr, allowed);
            }
        }
    }
}

RUVIA_TEST(response_trailer_wire_names_require_tokens_before_the_separator) {
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    for (const std::size_t length : {1U, 2U, 3U, 4U, 5U, 8U, 9U, 16U, 64U, 128U}) {
        for (const auto position : {std::size_t{0}, length / 2, length - 1}) {
            for (unsigned byte = 0; byte < 256; ++byte) {
                std::string name(length, 'x');
                name[position] = static_cast<char>(byte);
                const bool allowed = (byte >= '0' && byte <= '9') ||
                                     (byte >= 'A' && byte <= 'Z') ||
                                     (byte >= 'a' && byte <= 'z') ||
                                     punctuation.find(static_cast<char>(byte)) != std::string_view::npos;
                RUVIA_CHECK_EQ(ruvia::isValidHttpHeaderName(name), allowed);
                if (byte == ':') {
                    continue;  // A colon is the separator, not a candidate name byte.
                }
                const auto block = name + ": \tvalue\t \r\n";
                std::size_t delivered = 0;
                const auto valid = ruvia::detail::visitHttpResponseTrailerFields(
                    block, [&](std::string_view parsed_name, std::string_view parsed_value) {
                        ++delivered;
                        RUVIA_CHECK_EQ(parsed_name, std::string_view(name));
                        RUVIA_CHECK_EQ(parsed_value, "value");
                        return true;
                    });
                RUVIA_CHECK_EQ(valid, allowed);
                RUVIA_CHECK_EQ(delivered, allowed ? std::size_t{1} : std::size_t{0});
            }
        }
    }
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
