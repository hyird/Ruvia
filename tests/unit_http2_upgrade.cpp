#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "net/http2/Http2Upgrade.h"

namespace {

using ruvia::detail::http2DecodeBase64Url;

std::string decode(std::string_view input, bool& ok) {
    std::pmr::string out(std::pmr::get_default_resource());
    ok = http2DecodeBase64Url(input, out);
    return std::string(out.data(), out.size());
}

}  // namespace

RUVIA_TEST(http2_base64url_decode_valid_settings) {
    bool ok = false;
    // "AAQAAQAA" -> the 6-byte SETTINGS entry {00 04 00 01 00 00}
    // (SETTINGS_INITIAL_WINDOW_SIZE = 0x00010000).
    const auto decoded = decode("AAQAAQAA", ok);
    RUVIA_CHECK(ok);
    RUVIA_CHECK_EQ(decoded.size(), std::size_t{6});
    RUVIA_CHECK_EQ(static_cast<unsigned char>(decoded[0]), 0x00U);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(decoded[1]), 0x04U);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(decoded[3]), 0x01U);
}

RUVIA_TEST(http2_base64url_decode_rejects_bad_input) {
    bool ok = true;
    // Standard-base64 characters '+' and '/' are not valid base64url.
    (void)decode("AAQA+QAA", ok);
    RUVIA_CHECK(!ok);
    (void)decode("AAQA/QAA", ok);
    RUVIA_CHECK(!ok);
    // A length with remainder 1 mod 4 can never be valid base64.
    (void)decode("A", ok);
    RUVIA_CHECK(!ok);
    // Decodes cleanly but the SETTINGS payload is not a multiple of six bytes.
    (void)decode("AAQA", ok);  // 3 bytes
    RUVIA_CHECK(!ok);
    // A '=' pad in the middle (non-trailing) is invalid.
    (void)decode("AA=QAAAA", ok);
    RUVIA_CHECK(!ok);
}
