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

TEST(RuviaHttp, ip_address_format_view_borrows_output_not_address) {
    std::array<char, ruvia::ip_address::max_text_size> storage{};
    const auto formatted = ruvia::ip_address::ipv4({192, 0, 2, 1}).format_to(storage);
    EXPECT_EQ(formatted, "192.0.2.1");
    EXPECT_EQ(formatted.data(), storage.data());
}

TEST(RuviaHttp, ip_address_formats_maximal_scoped_ipv6) {
    const auto address = ruvia::ip_address::parse("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295");
    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(address->to_string(), "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295");
}

TEST(RuviaHttp, ip_address_classifies_native_and_mapped_addresses) {
    const auto v4 = ruvia::ip_address::parse("192.0.2.1");
    ASSERT_TRUE(v4);
    EXPECT_EQ(v4->ipv4_uint32(), 0xc0000201U);
    const auto mapped = ruvia::ip_address::parse("::ffff:192.0.2.1");
    ASSERT_TRUE(mapped);
    EXPECT_FALSE(mapped->ipv4_uint32());
    EXPECT_EQ(mapped->normalized_ipv4().ipv4_uint32(), v4->ipv4_uint32());
    struct classification final {
        std::string_view text;
        bool loopback;
        bool multicast;
        bool link_local;
        bool public_unicast;
    };
    for (const auto& entry : std::array{
             classification{"127.255.255.255", true, false, false, false},
             classification{"::ffff:127.0.0.1", true, false, false, false},
             classification{"::1", true, false, false, false},
             classification{"224.0.0.1", false, true, false, false},
             classification{"::ffff:239.255.255.255", false, true, false, false},
             classification{"ff02::1%2", false, true, false, false},
             classification{"169.254.0.1", false, false, true, false},
             classification{"::ffff:169.254.1.1", false, false, true, false},
             classification{"febf::1%2", false, false, true, false},
             classification{"fec0::1", false, false, false, false},
             classification{"8.8.8.8", false, false, false, true},
             classification{"::ffff:8.8.8.8", false, false, false, true},
             classification{"::ffff:8.8.8.8%2", false, false, false, false},
             classification{"2001:4860:4860::8888", false, false, false, true},
             classification{"2001:4860::1%2", false, false, false, false}
         }) {
        SCOPED_TRACE(entry.text);
        const auto address = ruvia::ip_address::parse(entry.text);
        ASSERT_TRUE(address);
        EXPECT_EQ(address->is_loopback(), entry.loopback);
        EXPECT_EQ(address->is_multicast(), entry.multicast);
        EXPECT_EQ(address->is_link_local(), entry.link_local);
        EXPECT_EQ(address->is_public_unicast(), entry.public_unicast);
    }
}

TEST(RuviaHttp, ip_address_public_unicast_excludes_special_purpose_space) {
    for (const std::string_view text : {"0.0.0.0", "0.255.255.255", "10.255.255.255", "100.64.0.0", "100.127.255.255", "127.0.0.1", "169.254.0.0", "172.16.0.0", "172.31.255.255", "192.0.0.8", "192.0.0.11", "192.0.2.1", "192.88.99.1", "192.168.255.255", "198.18.0.0", "198.19.255.255", "198.51.100.1", "203.0.113.1", "224.0.0.0", "240.0.0.1", "255.255.255.255", "::", "::1", "::192.0.2.1", "64:ff9b::808:808", "100::1", "2001::1", "2001:1ff:ffff::1", "2001:db8::1", "2002:808:808::1", "3ffe::1", "3fff:fff::1", "4000::1", "fc00::1", "fe80::1", "fec0::1", "ff00::1"}) {
        SCOPED_TRACE(text);
        const auto address = ruvia::ip_address::parse(text);
        ASSERT_TRUE(address);
        EXPECT_FALSE(address->is_public_unicast());
    }
    for (const std::string_view text : {"1.0.0.1", "100.63.255.255", "100.128.0.0", "172.15.255.255", "172.32.0.0", "192.0.0.9", "192.0.0.10", "198.17.255.255", "198.20.0.0", "2001:200::1", "2606:4700::1111"}) {
        SCOPED_TRACE(text);
        const auto address = ruvia::ip_address::parse(text);
        ASSERT_TRUE(address);
        EXPECT_TRUE(address->is_public_unicast());
    }
}

TEST(RuviaHttp, ip_cidr_preserves_family_prefix_and_inclusive_endpoints) {
    struct block final {
        std::string_view text;
        std::string_view first;
        std::string_view last;
        unsigned bits;
    };
    for (const auto& entry : std::array{
             block{"0.0.0.0/0", "0.0.0.0", "255.255.255.255", 0},
             block{"128.0.0.0/1", "128.0.0.0", "255.255.255.255", 1},
             block{"10.128.0.0/9", "10.128.0.0", "10.255.255.255", 9},
             block{"192.0.2.0/31", "192.0.2.0", "192.0.2.1", 31},
             block{"192.0.2.1", "192.0.2.1", "192.0.2.1", 32},
             block{"::/0", "::", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 0},
             block{"8000::/1", "8000::", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 1},
             block{"2001:db8:0:0:8000::/65", "2001:db8::8000:0:0:0", "2001:db8::ffff:ffff:ffff:ffff", 65},
             block{"2001:db8::/127", "2001:db8::", "2001:db8::1", 127},
             block{"::1/128", "::1", "::1", 128},
             block{"::ffff:192.0.2.0/120", "::ffff:192.0.2.0", "::ffff:192.0.2.255", 120}
         }) {
        SCOPED_TRACE(entry.text);
        const auto cidr = ruvia::ip_cidr::parse(entry.text);
        ASSERT_TRUE(cidr);
        EXPECT_EQ(cidr->prefix_bits(), entry.bits);
        EXPECT_EQ(cidr->network(), ruvia::ip_address::parse(entry.first));
        EXPECT_EQ(cidr->first_address(), ruvia::ip_address::parse(entry.first));
        EXPECT_EQ(cidr->last_address(), ruvia::ip_address::parse(entry.last));
        EXPECT_TRUE(cidr->contains(cidr->first_address()));
        EXPECT_TRUE(cidr->contains(cidr->last_address()));
    }
    const auto ipv4 = ruvia::ip_cidr::parse("192.0.2.0/24");
    ASSERT_TRUE(ipv4);
    EXPECT_TRUE(ipv4->contains("::ffff:192.0.2.1"));
    EXPECT_FALSE(ipv4->contains("::ffff:192.0.2.1%2"));
    EXPECT_FALSE(ipv4->contains("192.0.3.0"));
    EXPECT_NE(ipv4, ruvia::ip_cidr::parse("::ffff:192.0.2.0/120"));
    const auto ipv6 = ruvia::ip_cidr::parse("fe80::/10");
    ASSERT_TRUE(ipv6);
    EXPECT_TRUE(ipv6->contains("fe80::1%2"));
}

TEST(RuviaHttp, ip_cidr_rejects_host_bits_and_scoped_networks) {
    for (const std::string_view text : {"", "192.0.2.1/24", "10.127.0.0/8", "0.0.0.1/0", "192.0.2.0/33", "192.0.2.0/", "192.0.2.0/-1", "192.0.2.0/+1", "192.0.2.0/24/1", "::1/0", "2001:db8::1/64", "2001:db8:0:0:8000::1/65", "::/129", "::/128 ", "fe80::%0/10", "fe80::%2/10", "fe80::1%2", "::ffff:192.0.2.1/120"}) {
        SCOPED_TRACE(text);
        EXPECT_FALSE(ruvia::ip_cidr::parse(text));
    }
}
