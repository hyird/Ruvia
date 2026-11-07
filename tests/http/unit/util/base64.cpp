#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_base64.h"

namespace {

std::string encode(std::string_view input, ruvia::http_base64_padding padding = ruvia::http_base64_padding::required) {
    std::string output(ruvia::http_base64_encoded_size(input.size(), padding), '\0');
    ruvia::http_base64_encode(output.data(),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(input.data()), input.size()),
        ruvia::http_base64_charset::standard, padding);
    return output;
}

}  // namespace

TEST(Ruvia, http_base64_encodes_with_standard_alphabet_and_padding) {
    EXPECT_EQ(encode(""), std::string(""));
    EXPECT_EQ(encode("f"), std::string("Zg=="));
    EXPECT_EQ(encode("fo"), std::string("Zm8="));
    EXPECT_EQ(encode("foo"), std::string("Zm9v"));
    EXPECT_EQ(encode("foobar"), std::string("Zm9vYmFy"));
    EXPECT_EQ(encode("\xfb\xff"), std::string("+/8="));
}

TEST(Ruvia, http_base64_encode_accepts_output_overlapping_input) {
    constexpr std::string_view text = "abcdefghij";
    constexpr std::size_t input_start = 16;
    for (const auto padding : {ruvia::http_base64_padding::required, ruvia::http_base64_padding::forbidden}) {
        for (std::size_t size = 0; size <= text.size(); ++size) {
            const auto input = text.substr(0, size);
            const auto expected = encode(input, padding);
            // Every output start from wholly before to wholly after the input,
            // including in place and starts inside it.
            for (auto output_start = input_start - expected.size(); output_start <= input_start + size; ++output_start) {
                std::array<char, 48> buffer{};
                input.copy(buffer.data() + input_start, size);
                ruvia::http_base64_encode(buffer.data() + output_start,
                    std::span(reinterpret_cast<const std::uint8_t*>(buffer.data()) + input_start, size),
                    ruvia::http_base64_charset::standard, padding);
                EXPECT_EQ(std::string(buffer.data() + output_start, expected.size()), expected);
            }
        }
    }
}

TEST(Ruvia, http_base64_encoded_size_preserves_padding_at_numeric_boundaries) {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    constexpr auto maximum_groups = maximum / 4;
    constexpr auto maximum_input = maximum_groups * 3;
    constexpr auto maximum_output = maximum_groups * 4;
    constexpr auto small_output = ruvia::http_base64_encoded_size(4);
    EXPECT_EQ(small_output, std::size_t{8});
    for (const auto input_size : {maximum_input - 2, maximum_input - 1, maximum_input}) {
        EXPECT_EQ(ruvia::http_base64_encoded_size(input_size), maximum_output);
    }
    for (const auto input_size : {maximum_input + 1, maximum - 1, maximum}) {
        bool rejected = false;
        try {
            (void)ruvia::http_base64_encoded_size(input_size);
        } catch (const std::length_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, http_base64_unpadded_size_uses_actual_tail_at_numeric_boundary) {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    constexpr auto groups = maximum / 4;
    constexpr auto forbidden = ruvia::http_base64_padding::forbidden;
    EXPECT_EQ(ruvia::http_base64_encoded_size(3 * groups + 1, forbidden), maximum - 1);
    EXPECT_EQ(ruvia::http_base64_encoded_size(3 * groups + 2, forbidden), maximum);
    bool rejected = false;
    try {
        (void)ruvia::http_base64_encoded_size(3 * groups + 3, forbidden);
    } catch (const std::length_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, http_base64_decodes_single_characters_per_alphabet) {
    for (std::size_t i = 0; i < ruvia::http_base64_alphabet.size(); ++i) {
        EXPECT_EQ(ruvia::http_base64_decode_char(ruvia::http_base64_alphabet[i]), static_cast<int>(i));
        EXPECT_EQ(ruvia::http_base64_url_decode_char(ruvia::http_base64_url_alphabet[i]), static_cast<int>(i));
    }
    EXPECT_EQ(ruvia::http_base64_decode_char('='), -1);
    EXPECT_EQ(ruvia::http_base64_decode_char('-'), -1);
    EXPECT_EQ(ruvia::http_base64_url_decode_char('+'), -1);
    EXPECT_EQ(ruvia::http_base64_url_decode_char('/'), -1);
    EXPECT_EQ(ruvia::http_base64_url_decode_char('='), -1);
}

TEST(Ruvia, http_base64_decode_rejects_output_overwriting_unread_input) {
    constexpr auto standard = ruvia::http_base64_charset::standard;
    constexpr auto required = ruvia::http_base64_padding::required;
    const auto decoded_text = [](const std::uint8_t* bytes, std::size_t size) {
        return std::string(reinterpret_cast<const char*>(bytes), size);
    };

    std::uint8_t shifted[16] = {'Q', 'U', 'J', 'D', 'R', 'E', 'V', 'G'};
    const std::string_view shifted_input(reinterpret_cast<const char*>(shifted), 8);
    EXPECT_TRUE(!ruvia::http_base64_decode(shifted_input, std::span(shifted + 4, 6), standard, required));
    EXPECT_EQ(decoded_text(shifted, 8), std::string("QUJDREVG"));

    std::uint8_t in_place[8] = {'Q', 'U', 'J', 'D', 'R', 'E', 'V', 'G'};
    const std::string_view in_place_input(reinterpret_cast<const char*>(in_place), 8);
    const auto in_place_size = ruvia::http_base64_decode(in_place_input, std::span(in_place), standard, required);
    EXPECT_TRUE(in_place_size && *in_place_size == 6);
    EXPECT_EQ(decoded_text(in_place, 6), std::string("ABCDEF"));

    std::uint8_t after[16] = {'Q', 'U', 'J', 'D', 'R', 'E', 'V', 'G'};
    const std::string_view after_input(reinterpret_cast<const char*>(after), 8);
    const auto after_size = ruvia::http_base64_decode(after_input, std::span(after + 8, 8), standard, required);
    EXPECT_TRUE(after_size && *after_size == 6);
    EXPECT_EQ(decoded_text(after + 8, 6), std::string("ABCDEF"));
}
