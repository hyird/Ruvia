#include "tls_client_hello.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

TEST(RuviaHttpPublicApi, tls_client_hello_yields_ja3_ja4_and_peetprint) {
    // Construct a synthetic TLS 1.3 ClientHello handshake message containing
    // GREASE values, SNI ("example.com"), ALPN ("h2", "http/1.1"),
    // supported_groups (GREASE + x25519 29 + secp256r1 23), ec_point_formats (0),
    // signature_algorithms (0x0403, 0x0804), and supported_versions (GREASE, 0x0304, 0x0303).
    std::vector<std::uint8_t> body;
    const auto push_u8 = [&](std::uint8_t v) {
        body.push_back(v);
    };
    const auto push_u16 = [&](std::uint16_t v) {
        body.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xffU));
        body.push_back(static_cast<std::uint8_t>(v & 0xffU));
    };

    push_u16(0x0303);                   // legacy_version = TLS 1.2 (771)
    body.insert(body.end(), 32, 0x42);  // random
    push_u8(0);                         // empty session_id

    // Cipher suites: GREASE (0x0a0a), TLS_AES_128_GCM_SHA256 (0x1301 = 4865),
    // TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (0xc02f = 49199)
    push_u16(6);
    push_u16(0x0a0a);
    push_u16(0x1301);
    push_u16(0xc02f);

    // Compression methods: [0]
    push_u8(1);
    push_u8(0);

    std::vector<std::uint8_t> exts;
    const auto ext_u8 = [&](std::uint8_t v) {
        exts.push_back(v);
    };
    const auto ext_u16 = [&](std::uint16_t v) {
        exts.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xffU));
        exts.push_back(static_cast<std::uint8_t>(v & 0xffU));
    };

    // GREASE extension 0x1a1a
    ext_u16(0x1a1a);
    ext_u16(0);

    // server_name (0x0000): "example.com" (11 bytes)
    ext_u16(0x0000);
    ext_u16(16);
    ext_u16(14);
    ext_u8(0);
    ext_u16(11);
    for (const char ch : std::string_view("example.com")) {
        ext_u8(static_cast<std::uint8_t>(ch));
    }

    // supported_groups (0x000a): GREASE 0x2a2a, 29 (x25519), 23 (secp256r1)
    ext_u16(0x000a);
    ext_u16(8);
    ext_u16(6);
    ext_u16(0x2a2a);
    ext_u16(29);
    ext_u16(23);

    // ec_point_formats (0x000b): [0]
    ext_u16(0x000b);
    ext_u16(2);
    ext_u8(1);
    ext_u8(0);

    // signature_algorithms (0x000d): 0x0403 (1027), 0x0804 (2052)
    ext_u16(0x000d);
    ext_u16(6);
    ext_u16(4);
    ext_u16(0x0403);
    ext_u16(0x0804);

    // ALPN (0x0010): "h2", "http/1.1"
    ext_u16(0x0010);
    ext_u16(14);
    ext_u16(12);
    ext_u8(2);
    ext_u8('h');
    ext_u8('2');
    ext_u8(8);
    for (const char ch : std::string_view("http/1.1")) {
        ext_u8(static_cast<std::uint8_t>(ch));
    }

    // supported_versions (0x002b): GREASE 0x3a3a, 0x0304 (772), 0x0303 (771)
    ext_u16(0x002b);
    ext_u16(7);
    ext_u8(6);
    ext_u16(0x3a3a);
    ext_u16(0x0304);
    ext_u16(0x0303);

    push_u16(static_cast<std::uint16_t>(exts.size()));
    body.insert(body.end(), exts.begin(), exts.end());

    std::vector<std::byte> message;
    message.push_back(std::byte{0x01});
    message.push_back(static_cast<std::byte>((body.size() >> 16U) & 0xffU));
    message.push_back(static_cast<std::byte>((body.size() >> 8U) & 0xffU));
    message.push_back(static_cast<std::byte>(body.size() & 0xffU));
    for (const auto b : body) {
        message.push_back(static_cast<std::byte>(b));
    }

    const auto parsed = ruvia::parse_tls_client_hello(message);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->legacy_version(), 0x0303U);
    EXPECT_EQ(parsed->version(), 0x0304U);
    EXPECT_EQ(parsed->server_name(), "example.com");
    EXPECT_EQ(parsed->ja3_raw(), "771,4865-49199,0-10-11-13-16-43,29-23,0");
    EXPECT_EQ(parsed->ja3().size(), 32U);

    const auto tcp_ja4 = parsed->ja4(false);
    const auto quic_ja4 = parsed->ja4(true);
    EXPECT_TRUE(tcp_ja4.starts_with("t13d0206h2_"));
    EXPECT_TRUE(quic_ja4.starts_with("q13d0206h2_"));
    EXPECT_EQ(tcp_ja4.substr(1), quic_ja4.substr(1));
    EXPECT_EQ(parsed->peetprint(), "772-771|4865-49199|0-10-11-13-16-43|29-23|0|h2-http/1.1|1027-2052");
}
