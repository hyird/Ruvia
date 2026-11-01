#include <array>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/http_hex.h"

#include "test_harness.h"

RUVIA_TEST(hex_nibbles_decode_and_encode_ascii_digits) {
    RUVIA_CHECK_EQ(ruvia::http_decode_hex_nibble('0'), 0);
    RUVIA_CHECK_EQ(ruvia::http_decode_hex_nibble('a'), 10);
    RUVIA_CHECK_EQ(ruvia::http_decode_hex_nibble('F'), 15);
    RUVIA_CHECK_EQ(ruvia::http_decode_hex_nibble('g'), -1);
    RUVIA_CHECK_EQ(ruvia::http_lower_hex_digit(15), 'f');
    RUVIA_CHECK_EQ(ruvia::http_upper_hex_digit(15), 'F');
}

RUVIA_TEST(hex_encode_accepts_output_overlapping_input) {
    constexpr std::string_view text =
        "ab\x01\xff"
        "cdef";
    constexpr std::size_t input_start = 16;
    for (std::size_t size = 0; size <= text.size(); ++size) {
        const auto input = text.substr(0, size);
        std::string expected(2 * size, '\0');
        ruvia::http_encode_lower_hex(expected.data(), input);
        // Every output start from wholly before to wholly after the input,
        // including in place and starts inside it.
        for (auto output_start = input_start - expected.size(); output_start <= input_start + size; ++output_start) {
            std::array<char, 48> buffer{};
            input.copy(buffer.data() + input_start, size);
            const auto* const end =
                ruvia::http_encode_lower_hex(buffer.data() + output_start, std::string_view(buffer.data() + input_start, size));
            RUVIA_CHECK_EQ(end, buffer.data() + output_start + expected.size());
            RUVIA_CHECK_EQ(std::string(buffer.data() + output_start, expected.size()), expected);
        }
    }
}

RUVIA_TEST(hex_append_accepts_input_aliasing_the_output) {
    std::pmr::string whole("\x01\xab", std::pmr::new_delete_resource());
    whole.shrink_to_fit();
    ruvia::http_append_lower_hex(whole, std::string_view(whole));
    RUVIA_CHECK_EQ(std::string_view(whole), std::string_view("\x01\xab"
                                                             "01ab"));

    // A long prefix forces reallocation while the source is a strict subrange.
    std::pmr::string part(std::string(64, 'z') + "\xff", std::pmr::new_delete_resource());
    part.shrink_to_fit();
    const std::span<const unsigned char> tail(reinterpret_cast<const unsigned char*>(part.data()) + 63, 2);
    ruvia::http_append_lower_hex(part, tail);
    RUVIA_CHECK_EQ(std::string_view(part).substr(65), std::string_view("7aff"));

    // The terminating NUL is readable input: it ends where the digits begin.
    std::pmr::string terminated("A", std::pmr::new_delete_resource());
    terminated.shrink_to_fit();
    ruvia::http_append_lower_hex(terminated, std::string_view(terminated.data(), terminated.size() + 1));
    RUVIA_CHECK_EQ(std::string_view(terminated), std::string_view("A4100"));

    // A full string reallocates while the NUL-only input borrows the old buffer.
    std::pmr::string nul_only("A", std::pmr::new_delete_resource());
    nul_only.resize(nul_only.capacity(), 'B');
    const auto filled = nul_only.size();
    ruvia::http_append_lower_hex(nul_only, std::string_view(nul_only.data() + filled, 1));
    RUVIA_CHECK_EQ(nul_only.size(), filled + 2);
    RUVIA_CHECK_EQ(std::string_view(nul_only).substr(filled - 1), std::string_view("B00"));
}
