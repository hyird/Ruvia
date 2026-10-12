#include "ruvia/http/http_hex.h"

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include "test_harness.h"

RUVIA_TEST(hex_nibbles_decode_and_encode_ascii_digits) {
    RUVIA_CHECK_EQ(ruvia::http_decode_hex_nibble('0'), 0);
    RUVIA_CHECK_EQ(ruvia::http_decode_hex_nibble('a'), 10);
    RUVIA_CHECK_EQ(ruvia::http_decode_hex_nibble('F'), 15);
    RUVIA_CHECK_EQ(ruvia::http_decode_hex_nibble('g'), -1);
    RUVIA_CHECK_EQ(ruvia::http_lower_hex_digit(15), 'f');
    RUVIA_CHECK_EQ(ruvia::http_upper_hex_digit(15), 'F');
}

RUVIA_TEST(hex_append_accepts_input_aliasing_the_output) {
    std::pmr::string whole("\x01\xab", std::pmr::new_delete_resource());
    whole.shrink_to_fit();
    ruvia::http_append_lower_hex(whole, std::string_view(whole));
    RUVIA_CHECK_EQ(std::string_view(whole), std::string_view("\x01\xab" "01ab"));

    // A long prefix forces reallocation while the source is a strict subrange.
    std::pmr::string part(std::string(64, 'z') + "\xff", std::pmr::new_delete_resource());
    part.shrink_to_fit();
    const std::span<const unsigned char> tail(reinterpret_cast<const unsigned char*>(part.data()) + 63, 2);
    ruvia::http_append_lower_hex(part, tail);
    RUVIA_CHECK_EQ(std::string_view(part).substr(65), std::string_view("7aff"));
}
