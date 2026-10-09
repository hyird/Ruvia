#include <array>
#include <concepts>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/http_limits.h"

#include "test_harness.h"

namespace {

using ruvia::detail::check_http_response_trailer_section;
using ruvia::detail::http_response_trailer_block_valid;
using ruvia::detail::http_response_trailer_section_error;
using ruvia::detail::http_response_trailer_section_failure;
using ruvia::detail::http_response_trailer_section_result;
using ruvia::detail::is_forbidden_response_trailer_name;
using ruvia::detail::response_trailer_field_valid;
using ruvia::detail::visit_http_response_trailer_fields;

}  // namespace

RUVIA_TEST(response_trailer_name_is_a_token) {
    RUVIA_CHECK(response_trailer_field_valid("X-Trace-Id", ""));
    RUVIA_CHECK(response_trailer_field_valid("ETag", ""));
    // Empty, whitespace, and control bytes are not tokens.
    RUVIA_CHECK(!response_trailer_field_valid("", ""));
    RUVIA_CHECK(!response_trailer_field_valid("bad name", ""));
    RUVIA_CHECK(!response_trailer_field_valid(std::string_view("x\x01y", 3), ""));
    // A colon is not a tchar, so pseudo-headers can never pass (RFC 9113 8.1).
    RUVIA_CHECK(!response_trailer_field_valid(":status", ""));
}

RUVIA_TEST(response_trailer_value_rejects_splitting_bytes) {
    for (const auto value : {std::string_view("plain-value"), std::string_view{},
             std::string_view("v\x80z", 3), std::string_view("a\tb c", 5)}) {
        RUVIA_CHECK(response_trailer_field_valid("x-trace", value));
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
        RUVIA_CHECK(!response_trailer_field_valid("x-trace", value));
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
                RUVIA_CHECK_EQ(ruvia::is_valid_http_header_value(value), allowed);
                RUVIA_CHECK_EQ(response_trailer_field_valid("x-checksum", value), allowed);
                const std::array fields_value{ruvia::http_header_view("x-checksum", value)};
                const auto section = check_http_response_trailer_section(fields_value);
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
                RUVIA_CHECK_EQ(ruvia::is_valid_http_header_name(name), allowed);
                if (byte == ':') {
                    continue;  // A colon is the separator, not a candidate name byte.
                }
                const auto block = name + ": \tvalue\t \r\n";
                std::size_t delivered = 0;
                const auto valid = ruvia::detail::visit_http_response_trailer_fields(
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
    RUVIA_CHECK(is_forbidden_response_trailer_name("Transfer-Encoding"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Content-Length"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Host"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("TE"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Connection"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Proxy-Connection"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Trailer"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Upgrade"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Content-Type"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Content-Encoding"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Content-Range"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Set-Cookie"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Cache-Control"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Proxy-Authenticate"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Server"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Last-Modified"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Allow"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Access-Control-Allow-Origin"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Access-Control-Allow-Credentials"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Access-Control-Allow-Methods"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Access-Control-Allow-Headers"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Access-Control-Max-Age"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Access-Control-Expose-Headers"));
    for (const auto name : {"X-Content-Type-Options", "X-Frame-Options",
             "Strict-Transport-Security", "X-XSS-Protection", "Content-Security-Policy",
             "Content-Security-Policy-Report-Only", "Referrer-Policy", "Permissions-Policy",
             "Clear-Site-Data", "WWW-Authenticate", "Content-Disposition"}) {
        RUVIA_CHECK(is_forbidden_response_trailer_name(name));
    }
    // Response control data (RFC 9110 §6.5.1) must be processed before the content
    // and thus cannot be trailered: a recipient may discard trailers, silently
    // dropping the redirect/cache/auth-timing control.
    RUVIA_CHECK(is_forbidden_response_trailer_name("Age"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Date"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Vary"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Pragma"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Expires"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Warning"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Location"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("Retry-After"));
    // The check is case-insensitive.
    RUVIA_CHECK(is_forbidden_response_trailer_name("transfer-encoding"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("content-length"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("content-type"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("location"));
    RUVIA_CHECK(is_forbidden_response_trailer_name("retry-after"));
    // An ordinary field is allowed.
    RUVIA_CHECK(!is_forbidden_response_trailer_name("X-Trace-Id"));
    // RFC 9110 explicitly permits these fields in trailers.
    RUVIA_CHECK(!is_forbidden_response_trailer_name("ETag"));
    RUVIA_CHECK(!is_forbidden_response_trailer_name("Accept-Ranges"));
    RUVIA_CHECK(!is_forbidden_response_trailer_name("Server-Timing"));
}

RUVIA_TEST(response_trailer_field_combined_rule) {
    RUVIA_CHECK(response_trailer_field_valid("X-Trace-Id", "abc123"));
    RUVIA_CHECK(response_trailer_field_valid("Server-Timing", "db;dur=53"));
    // Invalid name.
    RUVIA_CHECK(!response_trailer_field_valid(":status", "200"));
    // Forbidden name.
    RUVIA_CHECK(!response_trailer_field_valid("Content-Length", "5"));
    RUVIA_CHECK(!response_trailer_field_valid("transfer-encoding", "chunked"));
    RUVIA_CHECK(!response_trailer_field_valid("Proxy-Connection", "keep-alive"));
    RUVIA_CHECK(!response_trailer_field_valid("Content-Type", "text/plain"));
    RUVIA_CHECK(!response_trailer_field_valid("Set-Cookie", "a=b"));
    RUVIA_CHECK(!response_trailer_field_valid("Allow", "GET, POST"));
    RUVIA_CHECK(!response_trailer_field_valid("Access-Control-Allow-Origin", "*"));
    RUVIA_CHECK(response_trailer_field_valid("Accept-Ranges", "bytes"));
    // Invalid value.
    RUVIA_CHECK(!response_trailer_field_valid("X-Trace-Id", std::string_view("a\r\nb", 4)));
    RUVIA_CHECK(!response_trailer_field_valid("X-Trace-Id", " abc"));
    RUVIA_CHECK(!response_trailer_field_valid("X-Trace-Id", "abc "));
    RUVIA_CHECK(!response_trailer_field_valid("X-Trace-Id", "\tabc"));
    RUVIA_CHECK(!response_trailer_field_valid("X-Trace-Id", "abc\t"));
}

RUVIA_TEST(response_trailer_block_parser_trims_and_uses_response_rules) {
    std::array<ruvia::http_header_view, 2> visited{};
    std::size_t count = 0;
    const auto ok = visit_http_response_trailer_fields(
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

    RUVIA_CHECK(http_response_trailer_block_valid("ETag: \"abc\"\r\n"));
    RUVIA_CHECK(!http_response_trailer_block_valid("Date: Sun, 06 Nov 1994 08:49:37 GMT"));
    RUVIA_CHECK(!http_response_trailer_block_valid("Content-Length: 5"));
    RUVIA_CHECK(!http_response_trailer_block_valid(" bad: fold"));
    RUVIA_CHECK(!http_response_trailer_block_valid(": value"));
}

RUVIA_TEST(response_trailer_section_validation_is_all_fields_or_none) {
    const std::array<ruvia::http_header_view, 2> valid{ruvia::http_header_view{"X-Trace-Id", "abc"},
        ruvia::http_header_view{"Server-Timing", "db;dur=5"}};
    const auto valid_result = check_http_response_trailer_section(valid);
    RUVIA_CHECK(valid_result.section() != nullptr);
    RUVIA_CHECK(valid_result.failure() == nullptr);

    const std::array<ruvia::http_header_view, 2> mixed{
        ruvia::http_header_view{"X-Trace-Id", "abc"}, ruvia::http_header_view{"Content-Length", "5"}};
    const auto mixed_result = check_http_response_trailer_section(mixed);
    RUVIA_CHECK(mixed_result.section() == nullptr);
    RUVIA_CHECK(mixed_result.failure() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(mixed_result.failure()->exception().what()),
        std::string_view("invalid HTTP response trailer section"));
    // An empty field sequence is the valid absence of a trailer section; the
    // submission API reports empty separately when asked to submit one.
    const auto empty_result = check_http_response_trailer_section({});
    RUVIA_CHECK(empty_result.section() != nullptr);
}

RUVIA_TEST(response_trailer_section_enforces_field_limits) {
    const std::string oversized_value(ruvia::max_http_header_bytes, 'x');
    const std::array<ruvia::http_header_view, 1> oversized{{
        {"X-Oversized", oversized_value},
    }};
    const auto oversized_result = check_http_response_trailer_section(oversized);
    RUVIA_CHECK(oversized_result.section() == nullptr);
    RUVIA_CHECK(oversized_result.failure() != nullptr);

    std::array<ruvia::http_header_view, ruvia::max_http_header_fields + 1> too_many{};
    for (auto& header : too_many) {
        header = {"X-Many", "value"};
    }
    const auto too_many_result = check_http_response_trailer_section(too_many);
    RUVIA_CHECK(too_many_result.section() == nullptr);
    RUVIA_CHECK(too_many_result.failure() != nullptr);
}
