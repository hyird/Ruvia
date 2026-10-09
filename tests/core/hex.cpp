#include "ruvia/core/hex.h"

#include "test_harness.h"

RUVIA_TEST(hex_nibbles_decode_and_encode_ascii_digits) {
    RUVIA_CHECK_EQ(ruvia::decode_hex_nibble('0'), 0);
    RUVIA_CHECK_EQ(ruvia::decode_hex_nibble('a'), 10);
    RUVIA_CHECK_EQ(ruvia::decode_hex_nibble('F'), 15);
    RUVIA_CHECK_EQ(ruvia::decode_hex_nibble('g'), -1);
    RUVIA_CHECK_EQ(ruvia::lower_hex_digit(15), 'f');
    RUVIA_CHECK_EQ(ruvia::upper_hex_digit(15), 'F');
}
