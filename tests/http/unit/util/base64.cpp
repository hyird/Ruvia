#include "test_harness.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/detail/util/HttpBase64.h"

namespace {

std::string httpBase64(std::string_view in) {
    std::string out(4 * ((in.size() + 2) / 3), '\0');
    ruvia::detail::encodeHttpBase64(out.data(),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(in.data()), in.size()));
    return out;
}

}  // namespace

RUVIA_TEST(http_base64_rfc4648_vectors) {
    RUVIA_CHECK_EQ(httpBase64(""), std::string(""));
    RUVIA_CHECK_EQ(httpBase64("f"), std::string("Zg=="));
    RUVIA_CHECK_EQ(httpBase64("fo"), std::string("Zm8="));
    RUVIA_CHECK_EQ(httpBase64("foo"), std::string("Zm9v"));
    RUVIA_CHECK_EQ(httpBase64("foob"), std::string("Zm9vYg=="));
    RUVIA_CHECK_EQ(httpBase64("fooba"), std::string("Zm9vYmE="));
    RUVIA_CHECK_EQ(httpBase64("foobar"), std::string("Zm9vYmFy"));
}

RUVIA_TEST(http_base64_binary_high_bytes) {
    const unsigned char bytes[] = {0xFF, 0x00, 0xFF};
    std::string out(4, '\0');
    ruvia::detail::encodeHttpBase64(
        out.data(), std::span<const std::uint8_t>(bytes, sizeof(bytes)));
    RUVIA_CHECK_EQ(out, std::string("/wD/"));
}
