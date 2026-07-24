#include "test_harness.h"

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace {

using ruvia::detail::ChunkSizeLineStatus;

// Parses a chunk-size line and returns the decoded size, or npos on any non-kOk
// status, so tests can assert size and rejection together.
std::size_t chunkSize(std::string_view line) {
    std::size_t size = 0;
    return ruvia::detail::parseHttpChunkSizeLine(line, size) == ChunkSizeLineStatus::kOk ? size : std::string_view::npos;
}

ChunkSizeLineStatus chunkStatus(std::string_view line) {
    std::size_t size = 0;
    return ruvia::detail::parseHttpChunkSizeLine(line, size);
}

}  // namespace

RUVIA_TEST(chunk_size_hex_decoding_all_cases) {
    RUVIA_CHECK_EQ(chunkSize("0"), std::size_t{0});
    RUVIA_CHECK_EQ(chunkSize("1a"), std::size_t{26});
    RUVIA_CHECK_EQ(chunkSize("FF"), std::size_t{255});       // uppercase
    RUVIA_CHECK_EQ(chunkSize("ff"), std::size_t{255});       // lowercase
    RUVIA_CHECK_EQ(chunkSize("dEaD"), std::size_t{0xDEAD});  // mixed case
    RUVIA_CHECK_EQ(chunkSize("1000"), std::size_t{0x1000});
}

RUVIA_TEST(chunk_size_with_extension_is_accepted) {
    // The size ends at the first non-hex byte; a valid chunk-extension follows.
    RUVIA_CHECK_EQ(chunkSize("10;ext=1"), std::size_t{16});
}

RUVIA_TEST(chunk_size_rejects_invalid) {
    // Empty / no leading hex digit.
    RUVIA_CHECK(chunkStatus("") == ChunkSizeLineStatus::kInvalidSize);
    RUVIA_CHECK(chunkStatus("g") == ChunkSizeLineStatus::kInvalidSize);
    RUVIA_CHECK(chunkStatus("xyz") == ChunkSizeLineStatus::kInvalidSize);
    // Leading OWS before the size is a smuggling vector and must be rejected.
    RUVIA_CHECK(chunkStatus(" 1") == ChunkSizeLineStatus::kInvalidSize);
}

RUVIA_TEST(chunk_extension_grammar_accepts_valid_forms) {
    // chunk-ext = *( BWS ";" BWS ext-name [ BWS "=" BWS ext-val ] ), where the
    // value is a token or a quoted-string. Exercise a bare name, multiple exts,
    // and a quoted value carrying spaces and an escaped quote.
    RUVIA_CHECK(chunkStatus("10;chunked") == ChunkSizeLineStatus::kOk);
    RUVIA_CHECK(chunkStatus("10;a=b;c=d") == ChunkSizeLineStatus::kOk);
    RUVIA_CHECK(chunkStatus("10 ; a = b") == ChunkSizeLineStatus::kOk);  // BWS around delimiters
    RUVIA_CHECK(chunkStatus("10;ext=\"a b\"") == ChunkSizeLineStatus::kOk);
    RUVIA_CHECK(chunkStatus("10;ext=\"a\\\"b\"") == ChunkSizeLineStatus::kOk);  // escaped quote
}

RUVIA_TEST(chunk_extension_grammar_rejects_malformed) {
    // A ';' with no ext-name, a missing name before '=', non-extension junk after
    // the size, an unterminated quoted-string, and a control byte in a value are
    // all rejected (the chunk-ext line is a request-smuggling-adjacent surface).
    RUVIA_CHECK(chunkStatus("10;") == ChunkSizeLineStatus::kInvalidExtension);
    RUVIA_CHECK(chunkStatus("10;=v") == ChunkSizeLineStatus::kInvalidExtension);
    RUVIA_CHECK(chunkStatus("10xyz") == ChunkSizeLineStatus::kInvalidExtension);
    RUVIA_CHECK(chunkStatus("10;a=\"unterminated") == ChunkSizeLineStatus::kInvalidExtension);
    RUVIA_CHECK(chunkStatus(std::string_view("10;a=b\x01", 7)) == ChunkSizeLineStatus::kInvalidExtension);
}

RUVIA_TEST(chunk_extension_quoted_value_rejects_control_bytes) {
    // A quoted-string ext-value takes a distinct code path from a bare token, so
    // the control-byte guard must hold inside the quotes too. A raw CR/LF (or any
    // control byte) in a quoted chunk-ext value would otherwise let an attacker
    // smuggle line-structure bytes past the validator -- both unescaped and
    // backslash-escaped forms must be rejected.
    RUVIA_CHECK(chunkStatus(std::string_view("10;a=\"b\x01\"", 8)) == ChunkSizeLineStatus::kInvalidExtension);
    // A bare CR/LF inside the quotes is the smuggling-relevant case.
    RUVIA_CHECK(chunkStatus(std::string_view("10;a=\"b\r\n\"", 9)) == ChunkSizeLineStatus::kInvalidExtension);
    // An escaped control byte ('\' followed by a control char) is rejected too.
    RUVIA_CHECK(chunkStatus(std::string_view("10;a=\"\\\x01\"", 8)) == ChunkSizeLineStatus::kInvalidExtension);
    // DEL (0x7F) is a control byte for this purpose and is rejected in quotes.
    RUVIA_CHECK(chunkStatus(std::string_view("10;a=\"\x7f\"", 7)) == ChunkSizeLineStatus::kInvalidExtension);
}

RUVIA_TEST(chunk_size_rejects_trailing_whitespace) {
    // RFC 9112 7.1: `chunk = chunk-size [ chunk-ext ] CRLF`. BWS is permitted only
    // before a ";" or "=" inside a chunk-ext, never as trailing space between the
    // chunk-size (or chunk-ext) and the CRLF. Accepting it is the trailing-edge twin
    // of the leading-OWS smuggling vector and must be rejected symmetrically.
    RUVIA_CHECK(chunkStatus("5 ") == ChunkSizeLineStatus::kInvalidExtension);               // SP after size
    RUVIA_CHECK(chunkStatus("5\t") == ChunkSizeLineStatus::kInvalidExtension);              // HTAB after size
    RUVIA_CHECK(chunkStatus("5  ") == ChunkSizeLineStatus::kInvalidExtension);              // multiple
    RUVIA_CHECK(chunkStatus("10;a=b ") == ChunkSizeLineStatus::kInvalidExtension);          // after ext value
    RUVIA_CHECK(chunkStatus("10;chunked ") == ChunkSizeLineStatus::kInvalidExtension);      // after bare ext
    RUVIA_CHECK(chunkStatus("10;ext=\"a b\" ") == ChunkSizeLineStatus::kInvalidExtension);  // after quoted value
    // The valid BWS-around-delimiters forms must still parse (no trailing space).
    RUVIA_CHECK(chunkStatus("10 ; a = b") == ChunkSizeLineStatus::kOk);
    RUVIA_CHECK(chunkStatus("10;a=b;c=d") == ChunkSizeLineStatus::kOk);
}

RUVIA_TEST(chunk_size_overflow_is_rejected) {
    // More hex digits than fit in size_t must report overflow, not wrap.
    RUVIA_CHECK(chunkStatus("ffffffffffffffff0") == ChunkSizeLineStatus::kOverflow);
}
