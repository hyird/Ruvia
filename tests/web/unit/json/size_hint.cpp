#include "test_harness.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "ruvia/web/detail/json/JsonEscape.h"

namespace {

using ruvia::detail::appendJsonString;
using ruvia::detail::jsonStringSizeHint;

// The hint must exactly equal what appendJsonString writes, so the output buffer
// is reserved precisely (undersizing would force a reallocation).
void checkConsistent(ruvia::testing::TestContext& ruvia_ctx, std::string_view value) {
    std::string out;
    appendJsonString(out, value);
    RUVIA_CHECK_EQ(jsonStringSizeHint(value), out.size());
}

std::string escaped(std::string_view value) {
    std::string out;
    appendJsonString(out, value);
    return out;
}

}  // namespace

RUVIA_TEST(json_string_size_hint_matches_output) {
    checkConsistent(ruvia_ctx, "");
    checkConsistent(ruvia_ctx, "plain text 123");
    checkConsistent(ruvia_ctx, "with \"quote\" and \\ backslash");

    // Every named short escape.
    std::string named = "a";
    named += '\b';
    named += '\f';
    named += '\n';
    named += '\r';
    named += '\t';
    named += 'z';
    checkConsistent(ruvia_ctx, named);

    // Non-named control bytes take the \u00XX form.
    std::string ctrl = "x";
    ctrl += '\x01';
    ctrl += '\x1f';
    checkConsistent(ruvia_ctx, ctrl);

    // An embedded NUL is a control byte.
    std::string withNull = "a";
    withNull += '\0';
    withNull += 'b';
    checkConsistent(ruvia_ctx, withNull);

    // High (UTF-8) bytes pass through as single bytes.
    std::string high = "a";
    high += static_cast<char>(0x80);
    high += static_cast<char>(0xff);
    high += 'b';
    checkConsistent(ruvia_ctx, high);

    // Exercise escapes immediately before, on, and after 16-byte SIMD blocks.
    std::string blocks(15, 'a');
    blocks.push_back('"');
    blocks.append(15, 'b');
    blocks.push_back('\\');
    blocks.push_back('\x01');
    blocks.append(17, 'c');
    checkConsistent(ruvia_ctx, blocks);
}

RUVIA_TEST(json_string_escape_output_content_is_exact) {
    // The size-hint test above only checks the output LENGTH; verify the actual
    // escaped bytes so a wrong-but-same-length escape (a swapped hex nibble, a
    // dropped char at a chunk boundary) can't slip through.

    // Plain text is wrapped in quotes, byte-for-byte.
    RUVIA_CHECK_EQ(escaped("abc"), std::string("\"abc\""));
    // Quote and backslash take their two-character escapes.
    RUVIA_CHECK_EQ(escaped("a\"b\\c"), std::string("\"a\\\"b\\\\c\""));
    // The five named control escapes are emitted in short form, not \u00XX.
    RUVIA_CHECK_EQ(escaped(std::string_view("\b\f\n\r\t", 5)), std::string("\"\\b\\f\\n\\r\\t\""));
    // Other control bytes take UPPERCASE-hex \u00XX with the correct nibbles.
    RUVIA_CHECK_EQ(escaped(std::string_view("\x00\x01\x1f", 3)), std::string("\"\\u0000\\u0001\\u001F\""));
    // Escapes interleaved with plain runs preserve every byte across chunk boundaries.
    RUVIA_CHECK_EQ(escaped("a\nb\"c"), std::string("\"a\\nb\\\"c\""));
    // High (UTF-8 lead/continuation) bytes pass through verbatim, never escaped.
    RUVIA_CHECK_EQ(escaped(std::string_view("\x80\xff", 2)), std::string("\"\x80\xff\""));
}
