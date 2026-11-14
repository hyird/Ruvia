#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>

#include "http_cache.h"
#include "http_header.h"
#include "http_proxy_forwarding.h"
#include "http_request.h"
#include "http_status.h"

TEST(RuviaHttpPublicApi, hop_by_hop_fields_and_connection_nomination) {
    const std::array<ruvia::http_header_view, 2> headers{{
        {"Connection", "keep-alive, X-Custom-Hop, Upgrade"},
        {"Connection", "close, X-Second-Hop"},
    }};
    const auto nominated = ruvia::parse_http_connection_nominated_fields(headers);
    EXPECT_TRUE(nominated.has_keep_alive());
    EXPECT_TRUE(nominated.has_close());
    EXPECT_TRUE(nominated.has_upgrade());
    EXPECT_TRUE(nominated.contains("x-custom-hop"));
    EXPECT_TRUE(nominated.contains("X-SECOND-HOP"));

    EXPECT_TRUE(ruvia::is_http_hop_by_hop_field_name("Proxy-Authorization"));
    EXPECT_TRUE(ruvia::is_http_hop_by_hop_field_name("Transfer-Encoding"));
    EXPECT_TRUE(ruvia::is_http_hop_by_hop_or_nominated_field("X-Custom-Hop", nominated));
    EXPECT_TRUE(ruvia::is_http_hop_by_hop_or_nominated_field("Upgrade", nominated, false));
    EXPECT_FALSE(ruvia::is_http_hop_by_hop_or_nominated_field("Upgrade", nominated, true));

    EXPECT_EQ(nominated.rebuild_connection_field(true, true), "Upgrade, x-custom-hop, x-second-hop");
    EXPECT_EQ(nominated.rebuild_connection_field(true, false), "Upgrade");
}

TEST(RuviaHttpPublicApi, proxy_target_max_forwards_method_semantics_and_wire_size) {
    EXPECT_EQ(ruvia::normalize_http_proxy_request_target("GET", "http://example.com:8080/a/b?x=1"), "/a/b?x=1");
    EXPECT_EQ(ruvia::normalize_http_proxy_request_target("GET", "https://example.com"), "/");
    EXPECT_EQ(ruvia::normalize_http_proxy_request_target("GET", "https://example.com?foo=bar"), "/?foo=bar");
    EXPECT_EQ(ruvia::normalize_http_proxy_request_target("OPTIONS", "http://example.com"), "*");
    EXPECT_EQ(ruvia::normalize_http_proxy_request_target("OPTIONS", "http://example.com?query"), "/?query");
    EXPECT_EQ(ruvia::normalize_http_proxy_request_target("GET", "/already/origin"), "/already/origin");

    const std::array<ruvia::http_header_view, 1> mf_five{{{"Max-Forwards", "5"}}};
    const auto parsed_mf = ruvia::parse_http_max_forwards(mf_five);
    EXPECT_TRUE(parsed_mf.is_valid());
    EXPECT_EQ(parsed_mf.current(), std::optional<std::uint64_t>(5));
    EXPECT_EQ(parsed_mf.decremented(), std::optional<std::uint64_t>(4));
    EXPECT_EQ(parsed_mf.format_decremented(), "4");

    const auto mf_zero = ruvia::parse_http_max_forwards("0");
    EXPECT_TRUE(mf_zero.exhausted());
    EXPECT_EQ(mf_zero.decremented(), std::nullopt);

    const std::array<ruvia::http_header_view, 2> mf_dup{{
        {"Max-Forwards", "5"},
        {"Max-Forwards", "4"},
    }};
    EXPECT_TRUE(ruvia::parse_http_max_forwards(mf_dup).is_invalid());
    EXPECT_TRUE(ruvia::parse_http_max_forwards("5, 4").is_invalid());

    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("GET"));
    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("HEAD"));
    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("DELETE"));
    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("OPTIONS"));
    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("TRACE"));
    EXPECT_TRUE(ruvia::http_method_sends_zero_content_length("POST"));
    EXPECT_TRUE(ruvia::http_method_sends_zero_content_length("PUT"));

    const std::array<ruvia::http_header_view, 2> wire_headers{{
        {"Content-Type", "text/plain"},
        {"Content-Length", "5"},
    }};
    // "HTTP/1.1 200 OK\r\n" (17) + "Content-Type: text/plain\r\n" (26) +
    // "Content-Length: 5\r\n" (19) + "\r\n" (2) = 64.
    EXPECT_EQ(ruvia::estimate_http1_response_head_bytes(ruvia::http_status::ok, wire_headers), 64U);
}
