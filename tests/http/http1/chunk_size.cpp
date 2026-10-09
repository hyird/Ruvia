#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

#include "ruvia/http/detail/parser/http_parser_syntax.h"

#include "test_harness.h"

namespace {

using ruvia::detail::chunk_size_line_status;

// Parses a chunk-size line and returns the decoded size, or npos on any non-ok
// status, so tests can assert size and rejection together.
std::size_t chunk_size(std::string_view line) {
    std::size_t size = 0;
    return ruvia::detail::parse_http_chunk_size_line(line, size) == chunk_size_line_status::ok
               ? size
               : std::string_view::npos;
}

chunk_size_line_status chunk_status(std::string_view line) {
    std::size_t size = 0;
    return ruvia::detail::parse_http_chunk_size_line(line, size);
}

}  // namespace

RUVIA_TEST(chunk_size_hex_decoding_all_cases) {
    RUVIA_CHECK_EQ(chunk_size("0"), std::size_t{0});
    RUVIA_CHECK_EQ(chunk_size("1a"), std::size_t{26});
    RUVIA_CHECK_EQ(chunk_size("FF"), std::size_t{255});       // uppercase
    RUVIA_CHECK_EQ(chunk_size("ff"), std::size_t{255});       // lowercase
    RUVIA_CHECK_EQ(chunk_size("dEaD"), std::size_t{0xDEAD});  // mixed case
    RUVIA_CHECK_EQ(chunk_size("1000"), std::size_t{0x1000});
}

RUVIA_TEST(chunk_size_with_extension_is_accepted) {
    // The size ends at the first non-hex byte; a valid chunk-extension follows.
    RUVIA_CHECK_EQ(chunk_size("10;ext=1"), std::size_t{16});
}

RUVIA_TEST(chunk_size_rejects_invalid) {
    // Empty / no leading hex digit.
    RUVIA_CHECK(chunk_status("") == chunk_size_line_status::invalid_size);
    RUVIA_CHECK(chunk_status("g") == chunk_size_line_status::invalid_size);
    RUVIA_CHECK(chunk_status("xyz") == chunk_size_line_status::invalid_size);
    // Leading OWS before the size is a smuggling vector and must be rejected.
    RUVIA_CHECK(chunk_status(" 1") == chunk_size_line_status::invalid_size);
}

RUVIA_TEST(chunk_extension_grammar_accepts_valid_forms) {
    // chunk-ext = *( BWS ";" BWS ext-name [ BWS "=" BWS ext-val ] ), where the
    // value is a token or a quoted-string. Exercise a bare name, multiple exts,
    // and a quoted value carrying spaces and an escaped quote.
    RUVIA_CHECK(chunk_status("10;chunked") == chunk_size_line_status::ok);
    RUVIA_CHECK(chunk_status("10;a=b;c=d") == chunk_size_line_status::ok);
    RUVIA_CHECK(chunk_status("10 ; a = b") == chunk_size_line_status::ok);  // BWS around delimiters
    RUVIA_CHECK(chunk_status("10;ext=\"a b\"") == chunk_size_line_status::ok);
    RUVIA_CHECK(chunk_status("10;ext=\"a\\\"b\"") == chunk_size_line_status::ok);  // escaped quote
}

RUVIA_TEST(chunk_extension_grammar_rejects_malformed) {
    // A ';' with no ext-name, a missing name before '=', non-extension junk after
    // the size, an unterminated quoted-string, and a control byte in a value are
    // all rejected (the chunk-ext line is a request-smuggling-adjacent surface).
    RUVIA_CHECK(chunk_status("10;") == chunk_size_line_status::invalid_extension);
    RUVIA_CHECK(chunk_status("10;=v") == chunk_size_line_status::invalid_extension);
    RUVIA_CHECK(chunk_status("10xyz") == chunk_size_line_status::invalid_extension);
    RUVIA_CHECK(chunk_status("10;a=\"unterminated") == chunk_size_line_status::invalid_extension);
    RUVIA_CHECK(
        chunk_status(std::string_view("10;a=b\x01", 7)) == chunk_size_line_status::invalid_extension);
}

RUVIA_TEST(chunk_extension_quoted_value_rejects_control_bytes) {
    // A quoted-string ext-value takes a distinct code path from a bare token, so
    // the control-byte guard must hold inside the quotes too. A raw CR/LF (or any
    // control byte) in a quoted chunk-ext value would otherwise let an attacker
    // smuggle line-structure bytes past the validator -- both unescaped and
    // backslash-escaped forms must be rejected.
    RUVIA_CHECK(chunk_status(std::string_view("10;a=\"b\x01\"", 8)) ==
                chunk_size_line_status::invalid_extension);
    // A bare CR/LF inside the quotes is the smuggling-relevant case.
    RUVIA_CHECK(chunk_status(std::string_view("10;a=\"b\r\n\"", 9)) ==
                chunk_size_line_status::invalid_extension);
    // An escaped control byte ('\' followed by a control char) is rejected too.
    RUVIA_CHECK(chunk_status(std::string_view("10;a=\"\\\x01\"", 8)) ==
                chunk_size_line_status::invalid_extension);
    // DEL (0x7F) is a control byte for this purpose and is rejected in quotes.
    RUVIA_CHECK(chunk_status(std::string_view("10;a=\"\x7f\"", 7)) ==
                chunk_size_line_status::invalid_extension);
}

RUVIA_TEST(chunk_size_rejects_trailing_whitespace) {
    // RFC 9112 7.1: `chunk = chunk-size [ chunk-ext ] CRLF`. BWS is permitted only
    // before a ";" or "=" inside a chunk-ext, never as trailing space between the
    // chunk-size (or chunk-ext) and the CRLF. Accepting it is the trailing-edge twin
    // of the leading-OWS smuggling vector and must be rejected symmetrically.
    RUVIA_CHECK(chunk_status("5 ") == chunk_size_line_status::invalid_extension);   // SP after size
    RUVIA_CHECK(chunk_status("5\t") == chunk_size_line_status::invalid_extension);  // HTAB after size
    RUVIA_CHECK(chunk_status("5  ") == chunk_size_line_status::invalid_extension);  // multiple
    RUVIA_CHECK(
        chunk_status("10;a=b ") == chunk_size_line_status::invalid_extension);  // after ext value
    RUVIA_CHECK(
        chunk_status("10;chunked ") == chunk_size_line_status::invalid_extension);  // after bare ext
    RUVIA_CHECK(chunk_status("10;ext=\"a b\" ") ==
                chunk_size_line_status::invalid_extension);  // after quoted value
    // The valid BWS-around-delimiters forms must still parse (no trailing space).
    RUVIA_CHECK(chunk_status("10 ; a = b") == chunk_size_line_status::ok);
    RUVIA_CHECK(chunk_status("10;a=b;c=d") == chunk_size_line_status::ok);
}

RUVIA_TEST(chunk_size_overflow_is_rejected) {
    // More hex digits than fit in size_t must report overflow, not wrap.
    RUVIA_CHECK(chunk_status("ffffffffffffffff0") == chunk_size_line_status::overflow);
}

RUVIA_TEST(chunk_size_numeric_boundaries_and_failure_preserve_output) {
    const std::string maximum(sizeof(std::size_t) * 2, 'f');
    std::size_t size = 0;
    RUVIA_CHECK(ruvia::detail::parse_http_chunk_size_line(maximum, size) == chunk_size_line_status::ok);
    RUVIA_CHECK_EQ(size, (std::numeric_limits<std::size_t>::max)());
    RUVIA_CHECK_EQ(chunk_size(std::string(128, '0') + "2A;name=value"), std::size_t{42});

    const auto check_failure = [&](std::string_view line, chunk_size_line_status expected) {
        std::size_t unchanged = 123;
        RUVIA_CHECK(ruvia::detail::parse_http_chunk_size_line(line, unchanged) == expected);
        RUVIA_CHECK_EQ(unchanged, std::size_t{123});
    };
    check_failure(maximum + "0", chunk_size_line_status::overflow);
    check_failure(maximum + "0;=bad", chunk_size_line_status::overflow);
    check_failure("+1", chunk_size_line_status::invalid_size);
    check_failure("-1", chunk_size_line_status::invalid_size);
    check_failure("0x10", chunk_size_line_status::invalid_extension);
    check_failure("1;=bad", chunk_size_line_status::invalid_extension);
}
