#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/Http2Upgrade.h"

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

RUVIA_TEST(http2_base64url_decode_multi_entry_settings) {
    bool ok = false;
    // Two 6-byte SETTINGS entries (12 bytes, output length a multiple of six):
    //   {00 04 00 01 00 00}  INITIAL_WINDOW_SIZE = 0x00010000
    //   {00 03 00 00 00 64}  MAX_CONCURRENT_STREAMS = 100
    // The single-entry vector only covers one 4-char group's worth of the decode
    // loop; a real HTTP2-Settings header carries several entries, so exercise the
    // loop across multiple groups and confirm the %6 length gate still passes.
    const auto decoded = decode("AAQAAQAAAAMAAABk", ok);
    RUVIA_CHECK(ok);
    RUVIA_CHECK_EQ(decoded.size(), std::size_t{12});
    const unsigned char expected[] = {0x00, 0x04, 0x00, 0x01, 0x00, 0x00,
                                      0x00, 0x03, 0x00, 0x00, 0x00, 0x64};
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        RUVIA_CHECK_EQ(static_cast<unsigned char>(decoded[i]), expected[i]);
    }
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
    // RFC 7540 3.2.1: HTTP2-Settings is UNPADDED base64url, so ANY '=' is invalid --
    // including trailing padding. "A===" is length 4 (so it clears the %4==1 guard)
    // but its only significant character is a 1-char final group, which is malformed;
    // it must be rejected rather than decode to an empty settings payload.
    (void)decode("A===", ok);
    RUVIA_CHECK(!ok);
}

RUVIA_TEST(http2_upgrade_response_serialization_is_http_owned) {
    std::string response;
    std::size_t parts = 0;
    ruvia::detail::forEachHttp2UpgradeResponsePart(
        [&response, &parts](std::string_view part) {
            response.append(part);
            ++parts;
        });
    RUVIA_CHECK_EQ(parts, std::size_t{3});
    RUVIA_CHECK(response.starts_with(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: h2c\r\n"
        "Server: ruvia\r\n"
        "Date: "));
    RUVIA_CHECK(response.ends_with("\r\n\r\n"));
}
