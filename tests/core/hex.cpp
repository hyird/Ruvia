#include "ruvia/core/Hex.h"

#include "test_harness.h"

RUVIA_TEST(hex_nibbles_decode_and_encode_ascii_digits) {
    RUVIA_CHECK_EQ(ruvia::decodeHexNibble('0'), 0);
    RUVIA_CHECK_EQ(ruvia::decodeHexNibble('a'), 10);
    RUVIA_CHECK_EQ(ruvia::decodeHexNibble('F'), 15);
    RUVIA_CHECK_EQ(ruvia::decodeHexNibble('g'), -1);
    RUVIA_CHECK_EQ(ruvia::lowerHexDigit(15), 'f');
    RUVIA_CHECK_EQ(ruvia::upperHexDigit(15), 'F');
}
