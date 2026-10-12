#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_base64.h"

#include "test_harness.h"

namespace {

std::string encode(std::string_view input) {
    std::string output(ruvia::http_base64_encoded_size(input.size()), '\0');
    ruvia::http_base64_encode(output.data(),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(input.data()), input.size()));
    return output;
}

}  // namespace

RUVIA_TEST(http_base64_encodes_with_standard_alphabet_and_padding) {
    RUVIA_CHECK_EQ(encode(""), std::string(""));
    RUVIA_CHECK_EQ(encode("f"), std::string("Zg=="));
    RUVIA_CHECK_EQ(encode("fo"), std::string("Zm8="));
    RUVIA_CHECK_EQ(encode("foo"), std::string("Zm9v"));
    RUVIA_CHECK_EQ(encode("foobar"), std::string("Zm9vYmFy"));
    RUVIA_CHECK_EQ(encode("\xfb\xff"), std::string("+/8="));
}

RUVIA_TEST(http_base64_encoded_size_preserves_padding_at_numeric_boundaries) {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    constexpr auto maximum_groups = maximum / 4;
    constexpr auto maximum_input = maximum_groups * 3;
    constexpr auto maximum_output = maximum_groups * 4;
    constexpr auto small_output = ruvia::http_base64_encoded_size(4);
    RUVIA_CHECK_EQ(small_output, std::size_t{8});
    for (const auto input_size : {maximum_input - 2, maximum_input - 1, maximum_input}) {
        RUVIA_CHECK_EQ(ruvia::http_base64_encoded_size(input_size), maximum_output);
    }
    for (const auto input_size : {maximum_input + 1, maximum - 1, maximum}) {
        bool rejected = false;
        try {
            (void)ruvia::http_base64_encoded_size(input_size);
        } catch (const std::length_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
}

RUVIA_TEST(http_base64_unpadded_size_uses_actual_tail_at_numeric_boundary) {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    constexpr auto groups = maximum / 4;
    constexpr auto forbidden = ruvia::http_base64_padding::forbidden;
    RUVIA_CHECK_EQ(ruvia::http_base64_encoded_size(3 * groups + 1, forbidden), maximum - 1);
    RUVIA_CHECK_EQ(ruvia::http_base64_encoded_size(3 * groups + 2, forbidden), maximum);
    bool rejected = false;
    try {
        (void)ruvia::http_base64_encoded_size(3 * groups + 3, forbidden);
    } catch (const std::length_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(http_base64_decodes_single_characters_per_alphabet) {
    for (std::size_t i = 0; i < ruvia::http_base64_alphabet.size(); ++i) {
        RUVIA_CHECK_EQ(ruvia::http_base64_decode_char(ruvia::http_base64_alphabet[i]), static_cast<int>(i));
        RUVIA_CHECK_EQ(ruvia::http_base64_url_decode_char(ruvia::http_base64_url_alphabet[i]), static_cast<int>(i));
    }
    RUVIA_CHECK_EQ(ruvia::http_base64_decode_char('='), -1);
    RUVIA_CHECK_EQ(ruvia::http_base64_decode_char('-'), -1);
    RUVIA_CHECK_EQ(ruvia::http_base64_url_decode_char('+'), -1);
    RUVIA_CHECK_EQ(ruvia::http_base64_url_decode_char('/'), -1);
    RUVIA_CHECK_EQ(ruvia::http_base64_url_decode_char('='), -1);
}
