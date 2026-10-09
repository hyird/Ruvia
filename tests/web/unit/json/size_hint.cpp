#include <cstddef>
#include <string>
#include <string_view>

#include "ruvia/web/detail/json/json_escape.h"

#include "test_harness.h"

namespace {

using ruvia::detail::append_json_string;
using ruvia::detail::json_string_size_hint;

// The hint must exactly equal what append_json_string writes, so the output buffer
// is reserved precisely (undersizing would force a reallocation).
void check_consistent(ruvia::testing::test_context& ruvia_ctx, std::string_view value) {
    std::string out;
    append_json_string(out, value);
    RUVIA_CHECK_EQ(json_string_size_hint(value), out.size());
}

std::string escaped(std::string_view value) {
    std::string out;
    append_json_string(out, value);
    return out;
}

}  // namespace

RUVIA_TEST(json_string_size_hint_matches_output) {
    check_consistent(ruvia_ctx, "");
    check_consistent(ruvia_ctx, "plain text 123");
    check_consistent(ruvia_ctx, "with \"quote\" and \\ backslash");

    // Every named short escape.
    std::string named = "a";
    named += '\b';
    named += '\f';
    named += '\n';
    named += '\r';
    named += '\t';
    named += 'z';
    check_consistent(ruvia_ctx, named);

    // Non-named control bytes take the \u00XX form.
    std::string ctrl = "x";
    ctrl += '\x01';
    ctrl += '\x1f';
    check_consistent(ruvia_ctx, ctrl);

    // An embedded NUL is a control byte.
    std::string with_null = "a";
    with_null += '\0';
    with_null += 'b';
    check_consistent(ruvia_ctx, with_null);

    // High (UTF-8) bytes pass through as single bytes.
    std::string high = "a";
    high += static_cast<char>(0x80);
    high += static_cast<char>(0xff);
    high += 'b';
    check_consistent(ruvia_ctx, high);

    // Exercise escapes immediately before, on, and after 16-byte SIMD blocks.
    std::string blocks(15, 'a');
    blocks.push_back('"');
    blocks.append(15, 'b');
    blocks.push_back('\\');
    blocks.push_back('\x01');
    blocks.append(17, 'c');
    check_consistent(ruvia_ctx, blocks);
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
    RUVIA_CHECK_EQ(
        escaped(std::string_view("\x00\x01\x1f", 3)), std::string("\"\\u0000\\u0001\\u001F\""));
    // Escapes interleaved with plain runs preserve every byte across chunk boundaries.
    RUVIA_CHECK_EQ(escaped("a\nb\"c"), std::string("\"a\\nb\\\"c\""));
    // High (UTF-8 lead/continuation) bytes pass through verbatim, never escaped.
    RUVIA_CHECK_EQ(escaped(std::string_view("\x80\xff", 2)), std::string("\"\x80\xff\""));
}
