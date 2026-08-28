#include "test_harness.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/core/detail/util/Base64.h"
#include "ruvia/core/detail/util/Base64Url.h"

namespace {

std::string b64(std::string_view in) {
    std::string out(ruvia::detail::base64EncodedSize(in.size()), '\0');
    ruvia::detail::encodeBase64(out.data(), std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(in.data()), in.size()));
    return out;
}

}  // namespace

// Core owns the framework-wide standard Base64 helpers. Protocol-specific
// encoders such as HTTP WebSocket accept hashing are tested with their owning
// target instead of importing core tests into tests/http.

RUVIA_TEST(base64_rfc4648_vectors) {
    RUVIA_CHECK_EQ(b64(""), std::string(""));
    RUVIA_CHECK_EQ(b64("f"), std::string("Zg=="));
    RUVIA_CHECK_EQ(b64("fo"), std::string("Zm8="));
    RUVIA_CHECK_EQ(b64("foo"), std::string("Zm9v"));
    RUVIA_CHECK_EQ(b64("foob"), std::string("Zm9vYg=="));
    RUVIA_CHECK_EQ(b64("fooba"), std::string("Zm9vYmE="));
    RUVIA_CHECK_EQ(b64("foobar"), std::string("Zm9vYmFy"));
}

RUVIA_TEST(base64_encoded_size) {
    using ruvia::detail::base64EncodedSize;
    RUVIA_CHECK_EQ(base64EncodedSize(0), std::size_t(0));
    RUVIA_CHECK_EQ(base64EncodedSize(1), std::size_t(4));
    RUVIA_CHECK_EQ(base64EncodedSize(2), std::size_t(4));
    RUVIA_CHECK_EQ(base64EncodedSize(3), std::size_t(4));
    RUVIA_CHECK_EQ(base64EncodedSize(4), std::size_t(8));
    RUVIA_CHECK_EQ(base64EncodedSize(32), std::size_t(44));  // HMAC-SHA256
}

RUVIA_TEST(base64_binary_high_bytes) {
    const unsigned char bytes[] = {0xFF, 0x00, 0xFF};
    std::string out(4, '\0');
    ruvia::detail::encodeBase64(out.data(), std::span<const std::uint8_t>(bytes, sizeof(bytes)));
    RUVIA_CHECK_EQ(out, std::string("/wD/"));
}

RUVIA_TEST(base64url_alphabet_values) {
    using ruvia::detail::decodeBase64UrlChar;
    // RFC 4648 §5: A-Z, a-z, 0-9, '-', '_'.
    RUVIA_CHECK_EQ(decodeBase64UrlChar('A'), 0);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('Z'), 25);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('a'), 26);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('z'), 51);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('0'), 52);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('9'), 61);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('-'), 62);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('_'), 63);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('+'), -1);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('/'), -1);
    RUVIA_CHECK_EQ(decodeBase64UrlChar('='), -1);
}
