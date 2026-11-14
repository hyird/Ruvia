#include <gtest/gtest.h>

#include <array>

#include "borrowed_text.h"
#include "http_client_ip.h"
#include "http_header.h"

TEST(RuviaHttpPublicApi, trusted_proxy_client_ip_extractor) {
    ruvia::http_client_ip_config config;
    config.enabled_ = true;
    config.trusted_proxies_.emplace_back("127.0.0.0/8");
    config.trusted_proxies_.emplace_back("10.0.0.0/8");
    config.trusted_proxies_.emplace_back("2001:db8:1::/48");
    config.forwarding_headers_.emplace_back("X-Forwarded-For");
    config.forwarding_headers_.emplace_back("Forwarded");
    config.forwarding_headers_.emplace_back("X-Real-IP", ruvia::http_forwarded_header_kind::single_ip);

    const ruvia::http_client_ip_extractor extractor(config);

    const std::array<ruvia::http_header_view, 2> headers{{
        {"X-Forwarded-For", "198.51.100.7, ::ffff:203.0.113.9, 10.1.2.3"},
        {"X-Real-IP", "192.0.2.55"},
    }};

    // Untrusted peer ignores forwarding headers.
    const auto direct = extractor.resolve("192.0.2.1", headers);
    ASSERT_TRUE(direct.has_value());
    EXPECT_FALSE(direct->from_forwarded_header());
    EXPECT_EQ(direct->address().to_string(), "192.0.2.1");

    // Trusted IPv4-mapped IPv6 peer strips 10.1.2.3 and normalizes ::ffff:203.0.113.9 to IPv4.
    const auto proxied = extractor.resolve("::ffff:127.0.0.1", headers);
    ASSERT_TRUE(proxied.has_value());
    EXPECT_TRUE(proxied->from_forwarded_header());
    EXPECT_EQ(proxied->address().family(), ruvia::ip_address_family::ipv4);
    EXPECT_EQ(proxied->address().to_string(), "203.0.113.9");

    // RFC 7239 Forwarded fallback when X-Forwarded-For is absent.
    const std::array<ruvia::http_header_view, 1> fwd_headers{{
        {"Forwarded", "for=\"[2001:db8:2::42]:8443\";proto=https, for=\"[2001:db8:1::1]\""},
    }};
    const auto from_rfc7239 = extractor.resolve("127.0.0.1", fwd_headers);
    ASSERT_TRUE(from_rfc7239.has_value());
    EXPECT_TRUE(from_rfc7239->from_forwarded_header());
    EXPECT_EQ(from_rfc7239->address().to_string(), "2001:db8:2::42");
}
