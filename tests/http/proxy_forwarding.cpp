#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http_cache.h"
#include "http_content_negotiation.h"
#include "http_header.h"
#include "http_proxy_forwarding.h"

namespace {
template <typename source_type>
concept parses_connection_fields = requires(source_type&& source) {
    ruvia::parse_http_connection_nominated_fields(std::forward<source_type>(source));
};
static_assert(parses_connection_fields<std::string&>);
static_assert(!parses_connection_fields<std::string>);
static_assert(parses_connection_fields<std::vector<ruvia::http_header>&>);
static_assert(!parses_connection_fields<std::vector<ruvia::http_header>>);
static_assert(!parses_connection_fields<std::initializer_list<ruvia::http_header_view>>);
}  // namespace

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

TEST(RuviaHttpPublicApi, proxy_target_and_max_forwards) {
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
}

TEST(RuviaHttpPublicApi, connection_nominations_borrow_original_bytes_and_report_terminal_failures) {
    std::string input = "X-Mixed-Case, x-mixed-case, close";
    auto nominated = ruvia::parse_http_connection_nominated_fields(input);
    ASSERT_EQ(nominated.status(), ruvia::http_connection_fields_status::valid);
    ASSERT_EQ(nominated.size(), 2U);
    EXPECT_EQ(nominated.tokens()[0], "X-Mixed-Case");
    EXPECT_EQ(nominated.tokens()[0].data(), input.data());
    EXPECT_TRUE(nominated.contains("X-MIXED-CASE"));
    nominated.update("bad token");
    EXPECT_EQ(nominated.status(), ruvia::http_connection_fields_status::invalid);
    nominated.update("X-Later");
    EXPECT_FALSE(nominated.contains("X-Later"));

    ruvia::http_connection_nominated_fields bounded;
    std::string many;
    for (std::size_t index = 0; index <= ruvia::http_connection_nominated_fields::capacity; ++index) {
        if (!many.empty()) {
            many += ',';
        }
        many += "x-" + std::to_string(index);
    }
    bounded.update(many);
    EXPECT_EQ(bounded.status(), ruvia::http_connection_fields_status::budget_exceeded);
    const std::string oversized(ruvia::http_connection_nominated_fields::max_input_bytes + 1, 'x');
    EXPECT_EQ(ruvia::parse_http_connection_nominated_fields(oversized).status(), ruvia::http_connection_fields_status::budget_exceeded);
}

TEST(RuviaHttpPublicApi, public_quality_and_media_range_parsers_preserve_wildcards) {
    EXPECT_EQ(ruvia::parse_http_quality_value("0"), 0);
    EXPECT_EQ(ruvia::parse_http_quality_value("0."), 0);
    EXPECT_EQ(ruvia::parse_http_quality_value("0.125"), 125);
    EXPECT_EQ(ruvia::parse_http_quality_value("1.000"), 1000);
    for (const auto invalid : {" 0.5", "0.5 ", ".5", "1.001", "0.1234", "-1", "\"0.5\""}) {
        EXPECT_FALSE(ruvia::parse_http_quality_value(invalid).has_value()) << invalid;
    }
    const auto wildcard = ruvia::parse_http_media_range("*/*;q=0.25");
    ASSERT_TRUE(wildcard.has_value());
    EXPECT_EQ(wildcard->type_, "*");
    EXPECT_EQ(wildcard->subtype_, "*");
    EXPECT_EQ(wildcard->quality_, 250);
    const auto type = ruvia::parse_http_media_range("Text/*;charset=utf-8;q=0");
    ASSERT_TRUE(type.has_value());
    EXPECT_EQ(type->type_, "Text");
    EXPECT_EQ(type->subtype_, "*");
    EXPECT_EQ(type->quality_, 0);
    EXPECT_EQ(type->parameter_count_, 1U);
    EXPECT_FALSE(ruvia::parse_http_media_range("*/json"));
    EXPECT_FALSE(ruvia::parse_http_media_range("text/*;q=bad"));
    EXPECT_FALSE(ruvia::parse_http_media_range("text/*;q=0.5;q=1"));
}
