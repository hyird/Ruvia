#include "http_cache.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "http_header.h"

TEST(RuviaHttpPublicApi, rfc9111_shared_cache_primitives) {
    const auto cc = ruvia::parse_cache_control("public, max-age=120, s-maxage=300, stale-while-revalidate=30, stale-if-error=600, must-revalidate");
    EXPECT_TRUE(cc.is_public());
    EXPECT_TRUE(cc.must_revalidate());
    EXPECT_EQ(cc.max_age(), std::optional<std::uint64_t>(120));
    EXPECT_EQ(cc.s_maxage(), std::optional<std::uint64_t>(300));
    EXPECT_EQ(cc.stale_while_revalidate(), std::optional<std::uint64_t>(30));
    EXPECT_EQ(cc.stale_if_error(), std::optional<std::uint64_t>(600));

    std::vector<ruvia::http_header> resp_headers;
    resp_headers.emplace_back("Cache-Control", "public, s-maxage=300, max-age=120, must-revalidate");
    resp_headers.emplace_back("Expires", "Wed, 21 Oct 2015 07:28:00 GMT");
    ruvia::rewrite_http_response_cache_max_age(resp_headers, 45);
    ASSERT_EQ(resp_headers.size(), 1U);
    EXPECT_EQ(resp_headers[0].name(), "Cache-Control");
    EXPECT_EQ(resp_headers[0].value(), "public, must-revalidate, max-age=45");

    // Vary normalization & matching.
    const std::array<ruvia::http_header_view, 2> vary_headers{{
        {"Vary", "Origin, Accept-Encoding"},
        {"Vary", "accept-language, ORIGIN"},
    }};
    const auto vary = ruvia::parse_http_vary(vary_headers);
    EXPECT_FALSE(vary.is_wildcard());
    ASSERT_EQ(vary.field_names().size(), 3U);
    EXPECT_EQ(vary.field_names()[0], "accept-encoding");
    EXPECT_EQ(vary.field_names()[1], "accept-language");
    EXPECT_EQ(vary.field_names()[2], "origin");

    const std::array<ruvia::http_header_view, 3> req1{{
        {"Origin", "https://example.com"},
        {"Accept-Encoding", "gzip,  br"},
        {"Accept-Language", "en-US"},
    }};
    const std::array<ruvia::http_header_view, 3> req2{{
        {"Accept-Language", "en-US"},
        {"Accept-Encoding", "gzip,\tbr"},
        {"Origin", "https://example.com"},
    }};
    EXPECT_TRUE(ruvia::http_vary_requests_match(vary, req1, req2));

    // RFC 9111 §4.2.3 Age calculation.
    const auto age = ruvia::compute_http_cache_age({
        .date_value_ = 1000,
        .age_value_ = 10,
        .request_time_ = 1005,
        .response_time_ = 1008,
        .now_ = 1020,
    });
    // apparent_age = 8, response_delay = 3, corrected_age_value = 13,
    // corrected_initial_age = 13, resident_time = 12 => 25.
    EXPECT_EQ(age, 25U);

    // 304 Not Modified sanitization & merging.
    EXPECT_TRUE(ruvia::is_http_304_forbidden_header("Content-Length"));
    EXPECT_TRUE(ruvia::is_http_304_forbidden_header("Content-Type"));
    EXPECT_FALSE(ruvia::is_http_304_forbidden_header("ETag"));
    EXPECT_FALSE(ruvia::is_http_304_forbidden_header("Cache-Control"));

    std::vector<ruvia::http_header> cached;
    cached.emplace_back("Content-Type", "text/plain");
    cached.emplace_back("Content-Length", "1024");
    cached.emplace_back("ETag", "\"v1\"");
    cached.emplace_back("Cache-Control", "max-age=60");

    const std::array<ruvia::http_header_view, 4> not_modified{{
        {"ETag", "\"v2\""},
        {"Cache-Control", "max-age=300"},
        {"Content-Length", "0"},
        {"Connection", "keep-alive"},
    }};
    ruvia::merge_http_304_response_headers(cached, not_modified);
    EXPECT_EQ(ruvia::parse_cache_control(cached).max_age(), std::optional<std::uint64_t>(300));
    bool has_text_plain = false;
    bool has_len_1024 = false;
    bool has_etag_v2 = false;
    for (const auto& field : cached) {
        if (field.name() == "Content-Type" && field.value() == "text/plain") {
            has_text_plain = true;
        }
        if (field.name() == "Content-Length" && field.value() == "1024") {
            has_len_1024 = true;
        }
        if (field.name() == "ETag" && field.value() == "\"v2\"") {
            has_etag_v2 = true;
        }
    }
    EXPECT_TRUE(has_text_plain);
    EXPECT_TRUE(has_len_1024);
    EXPECT_TRUE(has_etag_v2);
}

TEST(RuviaHttpPublicApi, targeted_cache_control_selects_first_valid_targeted_field) {
    // RFC 9213 §2.1: a targeted field is an SF-Dictionary; the first valid one in priority order wins.
    const auto cdn_cc = ruvia::parse_targeted_cache_control("max-age=60, max-age=300, stale-while-revalidate=45; param=1, no-store=?0, must-revalidate=?1");
    ASSERT_TRUE(cdn_cc.has_value());
    EXPECT_EQ(cdn_cc->max_age(), std::optional<std::uint64_t>(300));
    EXPECT_EQ(cdn_cc->stale_while_revalidate(), std::optional<std::uint64_t>(45));
    EXPECT_FALSE(cdn_cc->no_store());
    EXPECT_TRUE(cdn_cc->must_revalidate());

    // Invalid SF-Dictionary syntax (uppercase key or unterminated string) returns std::nullopt per RFC 9213 §2.1.
    EXPECT_FALSE(ruvia::parse_targeted_cache_control("MAX-AGE=60").has_value());
    EXPECT_FALSE(ruvia::parse_targeted_cache_control("").has_value());

    const std::array<ruvia::http_header_view, 3> response_headers{{
        {"Cache-Control", "private, no-store"},
        {"CDN-Cache-Control", "max-age=600, public"},
        {"Cloudflare-CDN-Cache-Control", "invalid=\"unterminated"},
    }};
    const std::array<std::string_view, 2> target_list{{
        "Cloudflare-CDN-Cache-Control",
        "CDN-Cache-Control",
    }};
    // Invalid first targeted header is ignored; second valid targeted header wins over Cache-Control.
    const auto selected = ruvia::resolve_targeted_cache_control(response_headers, target_list);
    EXPECT_TRUE(selected.targeted_);
    EXPECT_EQ(selected.selected_header_, "CDN-Cache-Control");
    EXPECT_TRUE(selected.control_.is_public());
    EXPECT_FALSE(selected.control_.no_store());
    EXPECT_EQ(selected.control_.max_age(), std::optional<std::uint64_t>(600));
}

TEST(RuviaHttpPublicApi, cache_control_field_qualified_private_and_no_cache_strip_named_fields) {
    // RFC 9111 §5.2.2.7 private="field" and §5.2.2.3 no-cache="field".
    const auto field_priv = ruvia::parse_cache_control("public, max-age=120, private=\"Set-Cookie, X-User-Secret\", no-cache=\"X-Auth-State\"");
    EXPECT_TRUE(field_priv.is_private());
    EXPECT_FALSE(field_priv.is_unqualified_private());
    EXPECT_TRUE(field_priv.is_field_qualified_private());
    EXPECT_EQ(field_priv.private_field_count(), 2U);
    EXPECT_TRUE(field_priv.has_private_field("set-cookie"));
    EXPECT_TRUE(field_priv.has_private_field("X-USER-SECRET"));
    EXPECT_FALSE(field_priv.has_private_field("Content-Type"));

    EXPECT_TRUE(field_priv.no_cache());
    EXPECT_FALSE(field_priv.is_unqualified_no_cache());
    EXPECT_TRUE(field_priv.is_field_qualified_no_cache());
    EXPECT_EQ(field_priv.no_cache_field_count(), 1U);
    EXPECT_TRUE(field_priv.has_no_cache_field("x-auth-state"));

    std::vector<ruvia::http_header> strip_headers;
    strip_headers.emplace_back("Content-Type", "application/json");
    strip_headers.emplace_back("Set-Cookie", "sid=123");
    strip_headers.emplace_back("X-User-Secret", "top-secret");
    strip_headers.emplace_back("X-Public", "kept");
    EXPECT_TRUE(ruvia::strip_http_cache_private_headers(strip_headers, field_priv));
    ASSERT_EQ(strip_headers.size(), 2U);
    EXPECT_EQ(strip_headers[0].name(), "Content-Type");
    EXPECT_EQ(strip_headers[1].name(), "X-Public");

    // Unqualified private alongside field-qualified private forbids shared-cache storage.
    const auto unqual_priv = ruvia::parse_cache_control("private=\"Set-Cookie\", private");
    EXPECT_TRUE(unqual_priv.is_unqualified_private());
    EXPECT_FALSE(unqual_priv.is_field_qualified_private());
    EXPECT_FALSE(ruvia::strip_http_cache_private_headers(strip_headers, unqual_priv));
}

TEST(Ruvia, parse_cache_control_flags_and_ages) {
    const auto cc = ruvia::parse_cache_control("public, max-age=60, s-maxage=120, stale-while-revalidate=30, immutable");
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::immutable));
    EXPECT_TRUE(!cc.has(static_cast<ruvia::cache_control_directive>(static_cast<std::uint16_t>(ruvia::cache_control_directive::public_value) | static_cast<std::uint16_t>(ruvia::cache_control_directive::immutable))));
    EXPECT_TRUE(cc.max_age().has_value());
    EXPECT_EQ(*cc.max_age(), std::uint64_t{60});
    EXPECT_EQ(*cc.s_maxage(), std::uint64_t{120});
    EXPECT_EQ(*cc.stale_while_revalidate(), std::uint64_t{30});
}

TEST(Ruvia, parse_cache_control_request_directives) {
    const auto request = ruvia::parse_cache_control("only-if-cached, max-age=0, min-fresh=15, max-stale=30");
    EXPECT_TRUE(request.has(ruvia::cache_control_directive::only_if_cached));
    EXPECT_EQ(request.max_age().value_or(1), std::uint64_t{0});
    EXPECT_EQ(request.min_fresh().value_or(0), std::uint64_t{15});
    EXPECT_EQ(request.max_stale().value_or(0), std::uint64_t{30});
    EXPECT_TRUE(!request.has(ruvia::cache_control_directive::max_stale_any));

    const auto any_stale = ruvia::parse_cache_control("max-stale");
    EXPECT_TRUE(any_stale.has(ruvia::cache_control_directive::max_stale_any));
    EXPECT_TRUE(!any_stale.max_stale().has_value());

    const auto invalid = ruvia::parse_cache_control("only-if-cached=yes, min-fresh=bad, max-stale=bad");
    EXPECT_TRUE(!invalid.has(ruvia::cache_control_directive::only_if_cached));
    EXPECT_TRUE(!invalid.min_fresh().has_value());
    EXPECT_TRUE(!invalid.max_stale().has_value());
    EXPECT_TRUE(!invalid.has(ruvia::cache_control_directive::max_stale_any));
}

TEST(Ruvia, parse_cache_control_request_freshness_uses_first_occurrence) {
    ruvia::cache_control_field_parser parser;
    parser.update("min-fresh=5, max-stale=10");
    parser.update("min-fresh=50, max-stale");
    const auto request = parser.finish();
    EXPECT_EQ(request.min_fresh().value_or(0), std::uint64_t{5});
    EXPECT_EQ(request.max_stale().value_or(0), std::uint64_t{10});
    EXPECT_TRUE(!request.has(ruvia::cache_control_directive::max_stale_any));
}

TEST(Ruvia, parse_cache_control_no_store_and_private) {
    const auto cc = ruvia::parse_cache_control("no-store, private, must-revalidate");
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::private_value));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::must_revalidate));
    EXPECT_TRUE(!cc.max_age().has_value());
}

TEST(Ruvia, parse_cache_control_no_transform_is_bare_and_quote_aware) {
    const auto present = ruvia::parse_cache_control("NO-TRANSFORM");
    EXPECT_TRUE(present.has(ruvia::cache_control_directive::no_transform));

    const auto quoted = ruvia::parse_cache_control(R"(extension="a, no-transform, b")");
    EXPECT_TRUE(!quoted.has(ruvia::cache_control_directive::no_transform));

    const auto qualified = ruvia::parse_cache_control("no-transform=ignored");
    EXPECT_TRUE(!qualified.has(ruvia::cache_control_directive::no_transform));
}

TEST(Ruvia, parse_cache_control_quoted_and_case_insensitive_and_unknown) {
    // Directive names are case-insensitive; a quoted delta-seconds is accepted; unknown ignored.
    const auto cc = ruvia::parse_cache_control("Max-Age=\"45\" , surrogate-control=foo, No-Cache");
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::no_cache));
    EXPECT_TRUE(cc.max_age().has_value());
    EXPECT_EQ(*cc.max_age(), std::uint64_t{45});
}

TEST(Ruvia, parse_cache_control_decodes_quoted_pairs_in_delta_seconds) {
    const auto cc = ruvia::parse_cache_control(R"(max-age="6\0", s-maxage="1\20", stale-while-revalidate="\30", stale-if-error="4\5")");
    EXPECT_EQ(cc.max_age().value_or(0), std::uint64_t{60});
    EXPECT_EQ(cc.s_maxage().value_or(0), std::uint64_t{120});
    EXPECT_EQ(cc.stale_while_revalidate().value_or(0), std::uint64_t{30});
    EXPECT_EQ(cc.stale_if_error().value_or(0), std::uint64_t{45});
}

TEST(Ruvia, parse_cache_control_rejects_bad_delta_seconds) {
    const auto cc = ruvia::parse_cache_control("max-age=abc, s-maxage=");
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_TRUE(!cc.s_maxage().has_value());
}

TEST(Ruvia, parse_cache_control_rejects_whitespace_around_equals) {
    const auto cc = ruvia::parse_cache_control(
        "max-age =60, s-maxage= 120, stale-while-revalidate = 30, "
        "stale-if-error=\t45, no-cache =\"Set-Cookie\", private = auth, "
        "no-cache= \"ETag\", private= auth");
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_TRUE(!cc.s_maxage().has_value());
    EXPECT_TRUE(!cc.stale_while_revalidate().has_value());
    EXPECT_TRUE(!cc.stale_if_error().has_value());
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_cache));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::private_value));

    // OWS around comma separators remains valid list framing.
    const auto separated = ruvia::parse_cache_control(" max-age=60 \t,\t no-store ");
    EXPECT_EQ(separated.max_age().value_or(0), std::uint64_t{60});
    EXPECT_TRUE(separated.has(ruvia::cache_control_directive::no_store));
}

TEST(Ruvia, parse_cache_control_freshness_uses_first_occurrence) {
    const auto duplicated = ruvia::parse_cache_control(
        "max-age=60, MAX-AGE=3600, s-maxage=120, s-maxage=7200, "
        "stale-while-revalidate=30, stale-while-revalidate=300, "
        "stale-if-error=45, stale-if-error=450");
    EXPECT_EQ(duplicated.max_age().value_or(0), std::uint64_t{60});
    EXPECT_EQ(duplicated.s_maxage().value_or(0), std::uint64_t{120});
    EXPECT_EQ(duplicated.stale_while_revalidate().value_or(0), std::uint64_t{30});
    EXPECT_EQ(duplicated.stale_if_error().value_or(0), std::uint64_t{45});

    // An invalid first occurrence cannot be repaired by a later value; caches
    // must not accidentally turn invalid freshness information into freshness.
    const auto invalid_first = ruvia::parse_cache_control("max-age=invalid, max-age=3600, s-maxage=, s-maxage=7200");
    EXPECT_TRUE(!invalid_first.max_age().has_value());
    EXPECT_TRUE(!invalid_first.s_maxage().has_value());
}

TEST(Ruvia, cache_control_field_parser_combines_repeated_lines) {
    ruvia::cache_control_field_parser parser;
    parser.update("public, max-age=invalid, s-maxage=120");
    parser.update(R"(extension="a, no-transform, b", no-transform, max-age=3600, s-maxage=7200)");

    const auto cc = parser.finish();
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::no_transform));
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_EQ(cc.s_maxage().value_or(0), std::uint64_t{120});
}

TEST(Ruvia, parse_cache_control_delta_seconds_overflow_saturates) {
    const auto cc = ruvia::parse_cache_control(
        "max-age=184467440737095516150, "
        "s-maxage=\"184467440737095516150\"");
    EXPECT_EQ(cc.max_age().value_or(0), (std::numeric_limits<std::uint64_t>::max)());
    EXPECT_EQ(cc.s_maxage().value_or(0), (std::numeric_limits<std::uint64_t>::max)());
}

TEST(Ruvia, parse_cache_control_does_not_split_quoted_extension_values) {
    const auto cc = ruvia::parse_cache_control("extension=\"a, public, max-age=999, b\", private");
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::private_value));

    // A quoted-pair keeps the escaped quote inside the extension value; its
    // commas likewise cannot introduce directives.
    const auto escaped = ruvia::parse_cache_control("extension=\"a\\\", no-store, b\", immutable");
    EXPECT_TRUE(!escaped.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(escaped.has(ruvia::cache_control_directive::immutable));

    // An unterminated quoted value is malformed through the end of the field,
    // so a comma within it must not accidentally enable caching directives.
    const auto unterminated = ruvia::parse_cache_control("extension=\"a, public, max-age=3600");
    EXPECT_TRUE(!unterminated.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(!unterminated.max_age().has_value());
}

TEST(Ruvia, parse_cache_control_ignores_arguments_on_bare_directives) {
    const auto cc = ruvia::parse_cache_control(
        "public=ignored, no-store=ignored, must-revalidate=x, "
        "proxy-revalidate=\"x\", no-transform=ignored, immutable=");
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::must_revalidate));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::proxy_revalidate));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_transform));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::immutable));

    // no-cache and private are different: their response forms can carry a
    // field-name list, so an argument does not invalidate the directive.
    const auto qualified = ruvia::parse_cache_control("no-cache=\"Set-Cookie\", private=\"Authorization\"");
    EXPECT_TRUE(qualified.has(ruvia::cache_control_directive::no_cache));
    EXPECT_TRUE(qualified.has(ruvia::cache_control_directive::private_value));
}

TEST(Ruvia, format_http_vary_cache_key_distinguishes_absent_from_empty_header) {
    const auto vary = ruvia::parse_http_vary("Accept-Language, X-Mode");
    const std::array<ruvia::http_header_view, 1> absent{
        ruvia::http_header_view{"X-Mode", "fast"},
    };
    const std::array<ruvia::http_header_view, 2> empty_value{
        ruvia::http_header_view{"Accept-Language", ""},
        ruvia::http_header_view{"X-Mode", "fast"},
    };
    const std::array<ruvia::http_header_view, 2> ows_only{
        ruvia::http_header_view{"Accept-Language", " \t "},
        ruvia::http_header_view{"X-Mode", "fast"},
    };
    const auto absent_key = ruvia::format_http_vary_cache_key(vary, absent);
    const auto empty_key = ruvia::format_http_vary_cache_key(vary, empty_value);
    const auto ows_key = ruvia::format_http_vary_cache_key(vary, ows_only);
    ASSERT_TRUE(absent_key.has_value());
    ASSERT_TRUE(empty_key.has_value());
    ASSERT_TRUE(ows_key.has_value());
    EXPECT_NE(*absent_key, *empty_key);
    EXPECT_EQ(*empty_key, *ows_key);
    EXPECT_FALSE(ruvia::http_vary_requests_match(vary, absent, empty_value));
    EXPECT_TRUE(ruvia::http_vary_requests_match(vary, empty_value, ows_only));
}
