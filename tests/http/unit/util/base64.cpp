#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "test_harness.h"
#include "util/http_base64.h"

namespace {

std::string http_base64(std::string_view in) {
    std::string out(4 * ((in.size() + 2) / 3), '\0');
    ruvia::detail::encode_http_base64(out.data(),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(in.data()), in.size()));
    return out;
}

}  // namespace

RUVIA_TEST(http_base64_rfc4648_vectors) {
    RUVIA_CHECK_EQ(http_base64(""), std::string(""));
    RUVIA_CHECK_EQ(http_base64("f"), std::string("Zg=="));
    RUVIA_CHECK_EQ(http_base64("fo"), std::string("Zm8="));
    RUVIA_CHECK_EQ(http_base64("foo"), std::string("Zm9v"));
    RUVIA_CHECK_EQ(http_base64("foob"), std::string("Zm9vYg=="));
    RUVIA_CHECK_EQ(http_base64("fooba"), std::string("Zm9vYmE="));
    RUVIA_CHECK_EQ(http_base64("foobar"), std::string("Zm9vYmFy"));
}

RUVIA_TEST(http_base64_binary_high_bytes) {
    const unsigned char bytes_value[] = {0xFF, 0x00, 0xFF};
    std::string out(4, '\0');
    ruvia::detail::encode_http_base64(
        out.data(), std::span<const std::uint8_t>(bytes_value, sizeof(bytes_value)));
    RUVIA_CHECK_EQ(out, std::string("/wD/"));
}
