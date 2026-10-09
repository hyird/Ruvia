#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/http/http_parse_error.h"

#include "parser/http_chunk_parser.h"
#include "test_harness.h"

namespace {

using ruvia::http_parse_error;
using ruvia::http_parse_protocol_error;
using ruvia::detail::http_chunk_scan_error;
using ruvia::detail::http_chunk_trailer_parser;
using ruvia::detail::validate_http_chunk_trailers;

}  // namespace

// Which fields a chunked trailer section may carry, and what it must reject.

RUVIA_TEST(chunk_trailers_accept_valid) {
    RUVIA_CHECK(!validate_http_chunk_trailers("").has_value());
    RUVIA_CHECK(!validate_http_chunk_trailers("X-Checksum: abc123\r\n").has_value());
    // A final line without a trailing CRLF is still complete.
    RUVIA_CHECK(!validate_http_chunk_trailers("X-Trace: v").has_value());
}

RUVIA_TEST(chunk_trailer_parser_exposes_validated_borrowed_fields) {
    http_chunk_trailer_parser parser("X-Trace: first\r\nServer-Timing:\tdb;dur=4  ");
    const auto first = parser.next();
    RUVIA_CHECK(first.field() != nullptr);
    if (const auto* field = first.field()) {
        RUVIA_CHECK_EQ(field->name(), std::string_view("X-Trace"));
        RUVIA_CHECK_EQ(field->value(), std::string_view("first"));
    }
    const auto second = parser.next();
    RUVIA_CHECK(second.field() != nullptr);
    if (const auto* field = second.field()) {
        RUVIA_CHECK_EQ(field->name(), std::string_view("Server-Timing"));
        RUVIA_CHECK_EQ(field->value(), std::string_view("db;dur=4"));
    }
    const auto terminal = parser.next();
    RUVIA_CHECK(terminal.end() != nullptr);
}

RUVIA_TEST(chunk_trailer_names_require_tokens_before_the_separator) {
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    for (const std::size_t length : {1U, 2U, 3U, 4U, 5U, 8U, 9U, 16U, 64U, 128U}) {
        for (const std::size_t position : {std::size_t{0}, length / 2, length - 1}) {
            for (unsigned byte = 0; byte < 256; ++byte) {
                if (byte == ':') {
                    continue;
                }
                const auto character = static_cast<char>(byte);
                const bool valid = (byte >= '0' && byte <= '9') ||
                                   (byte >= 'A' && byte <= 'Z') ||
                                   (byte >= 'a' && byte <= 'z') ||
                                   punctuation.find(character) != std::string_view::npos;
                std::string name(length, 'x');
                name[position] = character;
                const std::string wire = name + ": \tvalue:with:colons\t \r\nX-Next: done\r\n";
                http_chunk_trailer_parser parser(wire);
                const auto result_value = parser.next();
                RUVIA_CHECK_EQ(result_value.field() != nullptr, valid);
                if (valid && result_value.field()) {
                    RUVIA_CHECK_EQ(result_value.field()->name(), std::string_view(name));
                    RUVIA_CHECK_EQ(result_value.field()->name().data(), wire.data());
                    RUVIA_CHECK_EQ(result_value.field()->value(), std::string_view("value:with:colons"));
                    const auto next_value = parser.next();
                    RUVIA_CHECK(next_value.field() != nullptr);
                    if (next_value.field()) {
                        RUVIA_CHECK_EQ(next_value.field()->name(), std::string_view("X-Next"));
                    }
                    const auto end = parser.next();
                    RUVIA_CHECK(end.end() != nullptr);
                } else {
                    RUVIA_CHECK(result_value.failure() != nullptr);
                    if (result_value.failure()) {
                        RUVIA_CHECK(result_value.failure()->error() == http_chunk_scan_error::invalid_trailer);
                    }
                    const auto repeated = parser.next();
                    RUVIA_CHECK(repeated.failure() != nullptr);
                }
            }
        }
    }
}

RUVIA_TEST(chunk_trailer_values_trim_ows_and_preserve_accepted_bytes) {
    for (const std::size_t length : {1U, 2U, 3U, 31U, 32U, 33U, 64U, 4096U}) {
        for (const std::size_t position : {std::size_t{0}, length / 2, length - 1}) {
            for (unsigned byte = 0; byte < 256; ++byte) {
                const bool valid = byte == '\t' || (byte >= 32 && byte != 127);
                std::string value(length, 'x');
                value[position] = static_cast<char>(byte);
                const std::string wire = "X-Value: \t" + value + "\t \r\n";
                http_chunk_trailer_parser parser(wire);
                const auto result_value = parser.next();
                RUVIA_CHECK_EQ(result_value.field() != nullptr, valid);
                if (valid && result_value.field()) {
                    const auto first = value.find_first_not_of(" \t");
                    const auto last = value.find_last_not_of(" \t");
                    const auto expected = first == std::string::npos ? std::string_view{} : std::string_view(value).substr(first, last - first + 1);
                    RUVIA_CHECK_EQ(result_value.field()->value(), expected);
                } else {
                    RUVIA_CHECK(result_value.failure() != nullptr);
                    if (result_value.failure()) {
                        RUVIA_CHECK(result_value.failure()->error() == http_chunk_scan_error::invalid_trailer);
                    }
                }
            }
        }
    }
    RUVIA_CHECK(!validate_http_chunk_trailers("X-Empty:\r\nX-Ows: \t \r\n").has_value());
}

RUVIA_TEST(chunk_trailers_reject_malformed) {
    // Leading whitespace (obs-fold), missing colon, and an empty name are invalid.
    RUVIA_CHECK(validate_http_chunk_trailers(" X: y\r\n") == http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("no-colon\r\n") == http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers(":value\r\n") == http_chunk_scan_error::invalid_trailer);
    // A control byte in the value is rejected.
    RUVIA_CHECK(validate_http_chunk_trailers("X: a\x01"
                                             "b\r\n") == http_chunk_scan_error::invalid_trailer);
}

RUVIA_TEST(chunk_trailers_reject_forbidden_fields) {
    // Fields that govern framing/state must not appear in a trailer section.
    RUVIA_CHECK(
        validate_http_chunk_trailers("TE: trailers\r\n") == http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("Trailer: X\r\n") == http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(
        validate_http_chunk_trailers("Set-Cookie: a=b\r\n") == http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("Content-Encoding: gzip\r\n") ==
                http_chunk_scan_error::invalid_trailer);
}

RUVIA_TEST(chunk_trailers_reject_framing_and_routing_fields) {
    // The classic trailer-smuggling vectors: message-framing and routing/auth
    // headers injected in the trailer section, which a downstream parser might
    // honor after the head was already processed. These are rejected through the
    // classified-header path (distinct from the name-length switch exercised
    // above), so pin them explicitly -- dropping one reopens trailer smuggling.
    RUVIA_CHECK(
        validate_http_chunk_trailers("Content-Length: 10\r\n") == http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("Transfer-Encoding: chunked\r\n") ==
                http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(
        validate_http_chunk_trailers("Host: evil.example\r\n") == http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(
        validate_http_chunk_trailers("Connection: close\r\n") == http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("Authorization: Bearer x\r\n") ==
                http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(
        validate_http_chunk_trailers("Cookie: sid=1\r\n") == http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("Origin: https://app.example\r\n") ==
                http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("Access-Control-Request-Method: POST\r\n") ==
                http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("Access-Control-Request-Headers: X-One\r\n") ==
                http_chunk_scan_error::invalid_trailer);
    // The classification is case-insensitive, so a lowercase spelling is caught too.
    RUVIA_CHECK(
        validate_http_chunk_trailers("content-length: 10\r\n") == http_chunk_scan_error::invalid_trailer);
}

RUVIA_TEST(chunk_trailers_reject_remaining_forbidden_fields) {
    // The forbidden-trailer set has two tiers: the classified-header path and a
    // name-length switch for the less-common names. The tests above cover only a
    // few of each; the rest were unpinned even though the source comment warns
    // that "dropping one reopens trailer smuggling". Cover them all here.
    //
    // All protocols share this trailer policy. HTTP/2 and HTTP/3 also reject
    // Upgrade as a connection-specific field before applying trailer policy.
    RUVIA_CHECK(
        validate_http_chunk_trailers("Upgrade: websocket\r\n") == http_chunk_scan_error::invalid_trailer);
    // Proxy-Connection is forbidden both as a binary-protocol connection field
    // and by the shared trailer policy.
    RUVIA_CHECK(validate_http_chunk_trailers("Proxy-Connection: keep-alive\r\n") ==
                http_chunk_scan_error::invalid_trailer);

    // The name-length switch tier (each an RFC 7230 §4.1.2 / 7231 control or
    // routing/auth field that must not be delivered late in a trailer).
    RUVIA_CHECK(validate_http_chunk_trailers("Keep-Alive: timeout=5\r\n") ==
                http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(
        validate_http_chunk_trailers("Max-Forwards: 10\r\n") == http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("Cache-Control: no-cache\r\n") ==
                http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("Accept-Ranges: bytes\r\n") ==
                http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("Content-Range: bytes 0-1/2\r\n") ==
                http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("Proxy-Authenticate: Basic\r\n") ==
                http_chunk_scan_error::invalid_trailer);
    RUVIA_CHECK(validate_http_chunk_trailers("Proxy-Authorization: Basic eA==\r\n") ==
                http_chunk_scan_error::invalid_trailer);
    // A genuinely trailer-safe field is still accepted (negative control).
    RUVIA_CHECK(!validate_http_chunk_trailers("X-Checksum: abc\r\n").has_value());
}
