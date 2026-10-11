#include <cstdint>
#include <span>
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
