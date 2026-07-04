#include "test_harness.h"

#include <cstddef>
#include <cstdint>

#include "http/parser/HttpParserSyntax.h"

namespace {

using ruvia::detail::httpHexValue;
using ruvia::detail::isHttpFieldValueChar;
using ruvia::detail::isHttpHexDigit;
using ruvia::detail::isHttpTokenChar;

bool token(char c) noexcept { return isHttpTokenChar(static_cast<unsigned char>(c)); }
bool fieldValue(int c) noexcept { return isHttpFieldValueChar(static_cast<unsigned char>(c)); }

}  // namespace

RUVIA_TEST(http_token_char_table) {
    // tchar = ALPHA / DIGIT / a fixed symbol set (RFC 7230 3.2.6).
    for (const char c : {'a', 'z', 'A', 'Z', '0', '9', '!', '#', '$', '%', '&',
                         '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'}) {
        RUVIA_CHECK(token(c));
    }
    // Separators, whitespace and controls are not tchar.
    for (const char c : {'(', ')', '<', '>', '@', ',', ';', ':', '\\', '"', '/',
                         '[', ']', '?', '=', '{', '}', ' ', '\t'}) {
        RUVIA_CHECK(!token(c));
    }
    RUVIA_CHECK(!isHttpTokenChar(0));
    RUVIA_CHECK(!isHttpTokenChar(0x7f));
    RUVIA_CHECK(!isHttpTokenChar(0x80));
}

RUVIA_TEST(http_field_value_char_table) {
    // Visible ASCII, space and HTAB are field-value chars; obs-text too.
    for (const int c : {int{'a'}, int{'~'}, int{'0'}, int{' '}, int{'\t'}, int{'!'},
                        int{'@'}, 0x21, 0x7e, 0x80, 0xff}) {
        RUVIA_CHECK(fieldValue(c));
    }
    // CR, LF, NUL, other controls and DEL are rejected (response-splitting bytes).
    for (const int c : {0x00, 0x0d, 0x0a, 0x01, 0x08, 0x0b, 0x0c, 0x1f, 0x7f}) {
        RUVIA_CHECK(!fieldValue(c));
    }
}

RUVIA_TEST(http_hex_digit_and_value) {
    for (const char c : {'0', '9', 'A', 'F', 'a', 'f'}) {
        RUVIA_CHECK(isHttpHexDigit(static_cast<unsigned char>(c)));
    }
    for (const char c : {'g', 'G', '/', ':', ' ', 'z'}) {
        RUVIA_CHECK(!isHttpHexDigit(static_cast<unsigned char>(c)));
    }

    RUVIA_CHECK_EQ(httpHexValue('0'), std::uint8_t{0});
    RUVIA_CHECK_EQ(httpHexValue('9'), std::uint8_t{9});
    RUVIA_CHECK_EQ(httpHexValue('A'), std::uint8_t{10});
    RUVIA_CHECK_EQ(httpHexValue('F'), std::uint8_t{15});
    RUVIA_CHECK_EQ(httpHexValue('a'), std::uint8_t{10});
    RUVIA_CHECK_EQ(httpHexValue('f'), std::uint8_t{15});
}

RUVIA_TEST(request_header_kind_known_slot) {
    using ruvia::detail::kRequestHeaderKindCount;
    using ruvia::detail::RequestHeaderKind;
    using ruvia::detail::requestHeaderKindKnownSlot;
    // kOther has no cache slot -> the sentinel count; every real kind maps to
    // (enum index - 1).
    RUVIA_CHECK_EQ(requestHeaderKindKnownSlot(RequestHeaderKind::kOther), kRequestHeaderKindCount);
    RUVIA_CHECK_EQ(requestHeaderKindKnownSlot(RequestHeaderKind::kAccept), std::size_t{0});
    RUVIA_CHECK_EQ(requestHeaderKindKnownSlot(RequestHeaderKind::kAcceptEncoding), std::size_t{1});
    RUVIA_CHECK_EQ(requestHeaderKindKnownSlot(RequestHeaderKind::kUserAgent),
                   kRequestHeaderKindCount - 2);
}
