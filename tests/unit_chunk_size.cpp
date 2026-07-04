#include "test_harness.h"

#include <cstddef>
#include <string_view>

#include "http/parser/HttpParserSyntax.h"

namespace {

using ruvia::detail::ChunkSizeLineStatus;

// Parses a chunk-size line and returns the decoded size, or npos on any non-kOk
// status, so tests can assert size and rejection together.
std::size_t chunkSize(std::string_view line) {
    std::size_t size = 0;
    return ruvia::detail::parseHttpChunkSizeLine(line, size) == ChunkSizeLineStatus::kOk
        ? size
        : std::string_view::npos;
}

ChunkSizeLineStatus chunkStatus(std::string_view line) {
    std::size_t size = 0;
    return ruvia::detail::parseHttpChunkSizeLine(line, size);
}

}  // namespace

RUVIA_TEST(chunk_size_hex_decoding_all_cases) {
    RUVIA_CHECK_EQ(chunkSize("0"), std::size_t{0});
    RUVIA_CHECK_EQ(chunkSize("1a"), std::size_t{26});
    RUVIA_CHECK_EQ(chunkSize("FF"), std::size_t{255});    // uppercase
    RUVIA_CHECK_EQ(chunkSize("ff"), std::size_t{255});    // lowercase
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

RUVIA_TEST(chunk_size_overflow_is_rejected) {
    // More hex digits than fit in size_t must report overflow, not wrap.
    RUVIA_CHECK(chunkStatus("ffffffffffffffff0") == ChunkSizeLineStatus::kOverflow);
}
