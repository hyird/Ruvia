#include "test_harness.h"

#include <cstdint>

#include "ruvia/web/detail/json/JsonEscape.h"

namespace {

using ruvia::detail::jsonHexDigit;
using ruvia::detail::jsonNeedsEscape;

}  // namespace

RUVIA_TEST(json_hex_digit_uppercase) {
    RUVIA_CHECK_EQ(jsonHexDigit(0), '0');
    RUVIA_CHECK_EQ(jsonHexDigit(9), '9');
    RUVIA_CHECK_EQ(jsonHexDigit(10), 'A');
    RUVIA_CHECK_EQ(jsonHexDigit(15), 'F');
}

RUVIA_TEST(json_needs_escape) {
    // Quote, backslash, and control bytes (< 0x20) must be escaped (RFC 8259).
    RUVIA_CHECK(jsonNeedsEscape('"'));
    RUVIA_CHECK(jsonNeedsEscape('\\'));
    RUVIA_CHECK(jsonNeedsEscape(0x00));
    RUVIA_CHECK(jsonNeedsEscape('\n'));
    RUVIA_CHECK(jsonNeedsEscape('\t'));
    RUVIA_CHECK(jsonNeedsEscape(0x1f));
    // Space and printable ASCII do not need escaping.
    RUVIA_CHECK(!jsonNeedsEscape(' '));   // 0x20
    RUVIA_CHECK(!jsonNeedsEscape('a'));
    RUVIA_CHECK(!jsonNeedsEscape('/'));   // forward slash is not required to be escaped
    // DEL and high (UTF-8) bytes pass through unescaped.
    RUVIA_CHECK(!jsonNeedsEscape(0x7f));
    RUVIA_CHECK(!jsonNeedsEscape(0x80));
    RUVIA_CHECK(!jsonNeedsEscape(0xff));
}
