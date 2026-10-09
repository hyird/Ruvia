#include <cstdint>

#include "ruvia/web/detail/json/json_escape.h"

#include "test_harness.h"

namespace {

using ruvia::detail::json_hex_digit;
using ruvia::detail::json_needs_escape;

}  // namespace

RUVIA_TEST(json_hex_digit_uppercase) {
    RUVIA_CHECK_EQ(json_hex_digit(0), '0');
    RUVIA_CHECK_EQ(json_hex_digit(9), '9');
    RUVIA_CHECK_EQ(json_hex_digit(10), 'A');
    RUVIA_CHECK_EQ(json_hex_digit(15), 'F');
}

RUVIA_TEST(json_needs_escape) {
    // Quote, backslash, and control bytes (< 0x20) must be escaped (RFC 8259).
    RUVIA_CHECK(json_needs_escape('"'));
    RUVIA_CHECK(json_needs_escape('\\'));
    RUVIA_CHECK(json_needs_escape(0x00));
    RUVIA_CHECK(json_needs_escape('\n'));
    RUVIA_CHECK(json_needs_escape('\t'));
    RUVIA_CHECK(json_needs_escape(0x1f));
    // Space and printable ASCII do not need escaping.
    RUVIA_CHECK(!json_needs_escape(' '));  // 0x20
    RUVIA_CHECK(!json_needs_escape('a'));
    RUVIA_CHECK(!json_needs_escape('/'));  // forward slash is not required to be escaped
    // DEL and high (UTF-8) bytes pass through unescaped.
    RUVIA_CHECK(!json_needs_escape(0x7f));
    RUVIA_CHECK(!json_needs_escape(0x80));
    RUVIA_CHECK(!json_needs_escape(0xff));
}
