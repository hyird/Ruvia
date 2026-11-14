#include "ip_address.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "borrowed_text.h"

static_assert(std::is_trivially_copyable_v<ruvia::ip_address>);
static_assert(std::is_trivially_copyable_v<ruvia::ip_endpoint>);
static_assert(ruvia::ip_address{}.family() == ruvia::ip_address_family::ipv4);
static_assert(ruvia::ip_endpoint(ruvia::ip_address{}, 443).port() == 443);

TEST(RuviaHttp, ip_address_owns_network_order_bytes_and_endpoint_values) {
    std::array<std::uint8_t, 4> bytes{192, 0, 2, 1};
    const auto address = ruvia::ip_address::ipv4(bytes);
    bytes[0] = 127;
    EXPECT_EQ(address.family(), ruvia::ip_address_family::ipv4);
    ASSERT_EQ(address.bytes().size(), 4);
    EXPECT_EQ(address.bytes()[0], 192);
    EXPECT_EQ(address.bytes()[3], 1);
    EXPECT_EQ(address.scope_id(), 0);
    EXPECT_EQ(address.to_string(), "192.0.2.1");
    const ruvia::ip_endpoint endpoint(address, 65535);
    EXPECT_EQ(endpoint.address(), address);
    EXPECT_EQ(endpoint.port(), 65535);
    EXPECT_EQ(endpoint, ruvia::ip_endpoint(address, 65535));
    EXPECT_NE(endpoint, ruvia::ip_endpoint(address, 0));
    EXPECT_EQ(ruvia::ip_endpoint{}.port(), 0);
    EXPECT_EQ(ruvia::ip_address{}.to_string(), "0.0.0.0");
}

TEST(RuviaHttp, ip_address_formats_rfc5952_and_round_trips) {
    struct example final {
        std::string_view input;
        std::string_view canonical;
    };
    for (const auto& entry : std::array{example{"0.0.0.0", "0.0.0.0"}, example{"255.255.255.255", "255.255.255.255"}, example{"0:0:0:0:0:0:0:0", "::"}, example{"0:0:0:0:0:0:0:1", "::1"}, example{"2001:0DB8:0000:0000:0001:0000:0000:0001", "2001:db8::1:0:0:1"}, example{"2001:db8:1:2:3:4:0:0", "2001:db8:1:2:3:4::"}, example{"2001:db8:0:1:2:3:4:5", "2001:db8:0:1:2:3:4:5"}, example{"ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295"}, example{"::ffff:c000:201", "::ffff:192.0.2.1"}, example{"fe80::1%12", "fe80::1%12"}}) {
        SCOPED_TRACE(entry.input);
        const auto address = ruvia::ip_address::parse(entry.input);
        ASSERT_TRUE(address.has_value());
        EXPECT_EQ(address->to_string(), entry.canonical);
        EXPECT_EQ(ruvia::ip_address::parse(address->to_string()), address);
    }
}

TEST(RuviaHttp, ip_address_preserves_ipv6_family_scope_and_mapped_identity) {
    std::array<std::uint8_t, 16> bytes{};
    bytes[0] = 0xfe;
    bytes[1] = 0x80;
    bytes[15] = 1;
    const auto address = ruvia::ip_address::ipv6(bytes, 12);
    EXPECT_EQ(address.family(), ruvia::ip_address_family::ipv6);
    EXPECT_EQ(address.bytes().size(), 16);
    EXPECT_EQ(address.scope_id(), 12);
    EXPECT_EQ(address.to_string(), "fe80::1%12");
    EXPECT_NE(address, ruvia::ip_address::ipv6(bytes, 13));
    EXPECT_NE(ruvia::ip_address::parse("192.0.2.1"), ruvia::ip_address::parse("::ffff:192.0.2.1"));
}

TEST(RuviaHttp, ip_address_rejects_nonliteral_and_malformed_input) {
    for (const std::string_view text : {"", "localhost", "1", "192.0.2", "01.2.3.4", "192.0.2.256", "192.0.2.1:80", "192.0.2.1%1", "[::1]", "[::1]:80", " ::1", "::1 ", "1::2::3", "1:2:3:4:5:6:7:8:9", "::ffff:01.2.3.4", "fe80::1%", "fe80::1%eth0", "fe80::1%-1", "fe80::1%+1", "fe80::1%4294967296", "fe80::1%1%2"}) {
        SCOPED_TRACE(text);
        EXPECT_FALSE(ruvia::ip_address::parse(text).has_value());
    }
    EXPECT_FALSE(ruvia::ip_address::parse(std::string_view("::1\0suffix", 10)).has_value());
    constexpr std::string_view bounded = "192.0.2.1suffix";
    EXPECT_TRUE(ruvia::ip_address::parse(bounded.substr(0, 9)).has_value());
    EXPECT_FALSE(ruvia::ip_address::parse(bounded).has_value());
}

TEST(RuviaHttp, ip_address_formats_maximal_scoped_ipv6) {
    const auto address = ruvia::ip_address::parse("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295");
    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(address->to_string(), "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295");
}
