#include "http_cache.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http_header.h"
#include "http_response.h"

TEST(RuviaHttpPublicApi, cache_age_uses_only_the_first_received_member) {
    EXPECT_EQ(ruvia::parse_http_age(" \t12 \t, 99"), 12U);
    EXPECT_EQ(ruvia::parse_http_age("18446744073709551616, 1"), (std::numeric_limits<std::uint64_t>::max)());
    for (const std::string_view invalid : {", 99", "invalid, 99", "\"12\", 99", "12 3, 99"}) {
        EXPECT_EQ(ruvia::parse_http_age(invalid), std::nullopt) << invalid;
    }

    for (const auto& [first, expected] : {std::pair<std::string_view, std::uint64_t>{"12, 99", 17}, {"", 5}, {"bad, 12", 5}}) {
        const std::array<ruvia::http_header_view, 2> headers{{{"aGe", first}, {"Age", "99"}}};
        EXPECT_EQ(ruvia::compute_http_cache_age(headers, 100, 102, 105), expected) << first;
    }
}

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
    EXPECT_EQ(resp_headers[0].value(), "public, s-maxage=300, must-revalidate, max-age=45");

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
        {"ETag", "\"v1\""},
        {"Cache-Control", "max-age=300"},
        {"Content-Length", "0"},
        {"Connection", "keep-alive"},
    }};
    EXPECT_TRUE(ruvia::merge_http_304_response_headers(cached, not_modified));
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
        if (field.name() == "ETag" && field.value() == "\"v1\"") {
            has_etag_v2 = true;
        }
    }
    EXPECT_TRUE(has_text_plain);
    EXPECT_TRUE(has_len_1024);
    EXPECT_TRUE(has_etag_v2);
}

TEST(RuviaHttpPublicApi, vary_cache_key_normalizes_repeated_and_empty_field_values) {
    const auto vary = ruvia::parse_http_vary("Accept, X-Empty, X-Missing");
    const std::array<ruvia::http_header_view, 4> request{{
        {"Accept", " \tapplication/vnd.example+json;\t  charset=utf-8 \t"},
        {"X-Empty", "\t  \t"},
        {"Accept", "\ttext/plain;  \tcharset=us-ascii\t"},
        {"Other", "ignored"},
    }};
    const auto key = ruvia::format_http_vary_cache_key(vary, request);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(*key, "accept:1:application/vnd.example+json; charset=utf-8, text/plain; charset=us-ascii\nx-empty:1:\nx-missing:0\n");
}

TEST(RuviaHttpPublicApi, vary_names_sort_folded_ascii_and_own_only_distinct_names) {
    ruvia::http_vary_spec vary;
    {
        const std::string names = "X-Cache-Variation-NameZ, x-cache-variation-namea, X-Cache-Variation-Name_, x-cache-variation-name!, X-Cache-Variation-Name";
        vary.update(names);
    }
    vary.update("X-CACHE-VARIATION-NAMEA, x-cache-variation-namez, X-CACHE-VARIATION-NAME_, x-cache-variation-name!, X-CACHE-VARIATION-NAME");
    vary.update(vary.field_names().front());

    const auto names = vary.field_names();
    ASSERT_EQ(names.size(), 5U);
    EXPECT_EQ(names[0], "x-cache-variation-name");
    EXPECT_EQ(names[1], "x-cache-variation-name!");
    EXPECT_EQ(names[2], "x-cache-variation-name_");
    EXPECT_EQ(names[3], "x-cache-variation-namea");
    EXPECT_EQ(names[4], "x-cache-variation-namez");
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
    const std::array<ruvia::borrowed_text, 2> target_list{{
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

TEST(RuviaHttpPublicApi, header_rewrites_edit_header_blocks_and_responses_in_place) {
    ruvia::http_response response;
    response.header("Cache-Control", "public, s-maxage=300, max-age=120");
    response.header("Expires", "Wed, 21 Oct 2015 07:28:00 GMT");
    response.header("Content-Type", "text/plain");
    response.header("X-User-Secret", "top-secret");
    response.header("ETag", "\"v2\"");

    ruvia::rewrite_http_response_cache_max_age(response, 45);
    EXPECT_EQ(response.header("Cache-Control"), std::optional<std::string_view>("public, s-maxage=300, max-age=45"));
    EXPECT_FALSE(response.header("Expires").has_value());

    EXPECT_TRUE(ruvia::strip_http_cache_private_headers(response, ruvia::parse_cache_control("private=\"X-User-Secret\"")));
    EXPECT_FALSE(response.header("X-User-Secret").has_value());

    // A staged header block is edited the same way and leaves its source alone.
    auto staged = response.headers().clone();
    ruvia::sanitize_http_304_response_headers(staged);
    EXPECT_FALSE(staged.header("Content-Type").has_value());
    EXPECT_TRUE(staged.header("Cache-Control").has_value());
    EXPECT_TRUE(response.header("Content-Type").has_value());

    const std::array<ruvia::http_header_view, 2> not_modified{{
        {"ETag", "\"v2\""},
        {"Content-Type", "text/html"},
    }};
    EXPECT_TRUE(ruvia::merge_http_304_response_headers(response, not_modified));
    EXPECT_EQ(response.header("ETag"), std::optional<std::string_view>("\"v2\""));
    EXPECT_EQ(response.header("Content-Type"), std::optional<std::string_view>("text/plain"));
}

TEST(RuviaHttpPublicApi, merge_http_304_response_headers_reads_fields_that_view_part_of_the_cached_block) {
    std::vector<ruvia::http_header> cached;
    cached.emplace_back("X-First", "1");
    cached.emplace_back("ETag", "\"v1\"");
    cached.emplace_back("Cache-Control", "max-age=60");

    // The 304 fields are the last two cached lines themselves.
    EXPECT_TRUE(ruvia::merge_http_304_response_headers(cached, std::span<const ruvia::http_header>(cached).subspan(1)));
    ASSERT_EQ(cached.size(), 3U);
    EXPECT_EQ(ruvia::find_unique_http_header(cached, "X-First").value_, "1");
    EXPECT_EQ(ruvia::find_unique_http_header(cached, "ETag").value_, "\"v1\"");
    EXPECT_EQ(ruvia::find_unique_http_header(cached, "Cache-Control").value_, "max-age=60");
}

TEST(RuviaHttpPublicApi, multi_write_header_edit_leaves_an_http_response_unchanged_when_a_write_fails) {
    ruvia::http_response response;
    response.header("ETag", "\"v1\"");
    response.header("X-Trace", "kept");
    const auto field_count = response.headers().size();

    // The second Server line cannot join the first, so the merge fails after
    // ETag and the first Server line were already written to the candidate.
    const std::array<ruvia::http_header_view, 3> repeated_singleton{{
        {"ETag", "\"v1\""},
        {"Server", "edge-a"},
        {"Server", "edge-b"},
    }};
    EXPECT_THROW((void)ruvia::merge_http_304_response_headers(response, repeated_singleton), std::invalid_argument);
    EXPECT_EQ(response.header("ETag"), std::optional<std::string_view>("\"v1\""));
    EXPECT_FALSE(response.header("Server").has_value());
    EXPECT_EQ(response.headers().size(), field_count);
}

TEST(Ruvia, parse_cache_control_flags_and_ages) {
    const auto cc = ruvia::parse_cache_control("public, max-age=60, s-maxage=120, stale-while-revalidate=30, immutable");
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::public_));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::immutable));
    EXPECT_TRUE(!cc.has(static_cast<ruvia::cache_control_directive>(static_cast<std::uint16_t>(ruvia::cache_control_directive::public_) | static_cast<std::uint16_t>(ruvia::cache_control_directive::immutable))));
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
    const auto request = parser.control();
    EXPECT_EQ(request.min_fresh().value_or(0), std::uint64_t{5});
    EXPECT_EQ(request.max_stale().value_or(0), std::uint64_t{10});
    EXPECT_TRUE(!request.has(ruvia::cache_control_directive::max_stale_any));
}

TEST(Ruvia, parse_cache_control_no_store_and_private) {
    const auto cc = ruvia::parse_cache_control("no-store, private, must-revalidate");
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::private_));
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
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::private_));

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

    const auto cc = parser.control();
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::public_));
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
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::public_));
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::private_));

    // A quoted-pair keeps the escaped quote inside the extension value; its
    // commas likewise cannot introduce directives.
    const auto escaped = ruvia::parse_cache_control("extension=\"a\\\", no-store, b\", immutable");
    EXPECT_TRUE(!escaped.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(escaped.has(ruvia::cache_control_directive::immutable));

    // An unterminated quoted value is malformed through the end of the field,
    // so a comma within it must not accidentally enable caching directives.
    const auto unterminated = ruvia::parse_cache_control("extension=\"a, public, max-age=3600");
    EXPECT_TRUE(!unterminated.has(ruvia::cache_control_directive::public_));
    EXPECT_TRUE(!unterminated.max_age().has_value());
}

TEST(Ruvia, parse_cache_control_ignores_arguments_on_bare_directives) {
    const auto cc = ruvia::parse_cache_control(
        "public=ignored, no-store=ignored, must-revalidate=x, "
        "proxy-revalidate=\"x\", no-transform=ignored, immutable=");
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::public_));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::must_revalidate));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::proxy_revalidate));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_transform));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::immutable));

    // no-cache and private are different: their response forms can carry a
    // field-name list, so an argument does not invalidate the directive.
    const auto qualified = ruvia::parse_cache_control("no-cache=\"Set-Cookie\", private=\"Authorization\"");
    EXPECT_TRUE(qualified.has(ruvia::cache_control_directive::no_cache));
    EXPECT_TRUE(qualified.has(ruvia::cache_control_directive::private_));
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

TEST(RuviaHttpPublicApi, browser_ttl_removal_preserves_shared_and_explicit_expiry_policy) {
    EXPECT_EQ(ruvia::rewrite_http_cache_control_max_age("public, max-age=30, s-maxage=900, immutable", std::nullopt), "public, s-maxage=900, immutable");
    ruvia::http_response response;
    response.header("Cache-Control", "max-age=30");
    response.header("Expires", "Wed, 21 Oct 2015 07:28:00 GMT");
    ruvia::rewrite_http_response_cache_max_age(response, std::nullopt);
    EXPECT_FALSE(response.header("Cache-Control").has_value());
    EXPECT_TRUE(response.header("Expires").has_value());
}

TEST(RuviaHttpPublicApi, private_field_protection_rejects_without_partial_stripping) {
    ruvia::http_response response;
    response.header("X-Secret", "secret");
    response.header("ETag", "\"v1\"");
    const std::array<std::string_view, 1> protected_fields{"etag"};
    const auto control = ruvia::parse_cache_control("private=\"X-Secret, ETag\"");
    EXPECT_FALSE(ruvia::strip_http_cache_private_headers(response, control, protected_fields));
    EXPECT_TRUE(response.header("X-Secret").has_value());
    EXPECT_TRUE(response.header("ETag").has_value());
    EXPECT_TRUE(ruvia::strip_http_cache_private_headers(response, control));
    EXPECT_FALSE(response.header("X-Secret").has_value());
    EXPECT_FALSE(response.header("ETag").has_value());
}

TEST(RuviaHttpPublicApi, cache_freshening_validates_etags_and_content_encoding_before_mutation) {
    ruvia::http_response cached;
    cached.header("ETag", "\"v1\"");
    cached.header("Content-Encoding", "gzip");
    cached.header("Cache-Control", "max-age=30");
    const std::array<ruvia::http_header_view, 2> mismatch{{
        {"ETag", "\"v2\""},
        {"Cache-Control", "max-age=900"},
    }};
    EXPECT_FALSE(ruvia::merge_http_304_response_headers(cached, mismatch));
    EXPECT_EQ(cached.header("Cache-Control"), std::optional<std::string_view>("max-age=30"));
    const std::array<ruvia::http_header_view, 2> different_coding{{
        {"ETag", "\"v1\""},
        {"Content-Encoding", "br"},
    }};
    EXPECT_EQ(ruvia::match_http_cache_validators(cached, different_coding), ruvia::http_cache_validator_match::none);
    EXPECT_FALSE(ruvia::merge_http_304_response_headers(cached, different_coding));
    const std::array<ruvia::http_header_view, 3> weak{{
        {"ETag", "W/\"v1\""},
        {"Content-Encoding", "GZip"},
        {"Cache-Control", "max-age=60"},
    }};
    EXPECT_EQ(ruvia::match_http_cache_validators(cached, weak), ruvia::http_cache_validator_match::weak);
    EXPECT_TRUE(ruvia::merge_http_304_response_headers(cached, weak));
    EXPECT_EQ(cached.header("Cache-Control"), std::optional<std::string_view>("max-age=60"));
    EXPECT_EQ(cached.header("Content-Encoding"), std::optional<std::string_view>("gzip"));
    cached.header("ETag", "\"v1\"");
    EXPECT_TRUE(ruvia::weaken_http_cache_etag(cached));
    EXPECT_EQ(cached.header("ETag"), std::optional<std::string_view>("W/\"v1\""));
    EXPECT_TRUE(ruvia::weaken_http_cache_etag(cached));
    EXPECT_EQ(cached.header("ETag"), std::optional<std::string_view>("W/\"v1\""));
    const std::array<ruvia::http_header_view, 1> strong{{
        {"ETag", "\"v1\""},
    }};
    EXPECT_EQ(ruvia::match_http_cache_validators(cached, strong), ruvia::http_cache_validator_match::none);
}

TEST(RuviaHttpPublicApi, cache_date_validators_use_exact_match_and_sixty_second_strength) {
    ruvia::http_response cached;
    cached.header("Last-Modified", "Wed, 21 Oct 2015 07:28:00 GMT");
    cached.header("Date", "Wed, 21 Oct 2015 07:28:59 GMT");
    const std::array<ruvia::http_header_view, 1> validated{{
        {"Last-Modified", "Wed, 21 Oct 2015 07:28:00 GMT"},
    }};
    EXPECT_EQ(ruvia::match_http_cache_validators(cached, validated), ruvia::http_cache_validator_match::weak);
    cached.header("Date", "Wed, 21 Oct 2015 07:29:00 GMT");
    EXPECT_EQ(ruvia::match_http_cache_validators(cached, validated), ruvia::http_cache_validator_match::strong);
    const std::array<ruvia::http_header_view, 1> changed{{
        {"Last-Modified", "Wed, 21 Oct 2015 07:27:59 GMT"},
    }};
    EXPECT_EQ(ruvia::match_http_cache_validators(cached, changed), ruvia::http_cache_validator_match::none);
    const std::array<ruvia::http_header_view, 0> no_validator{};
    EXPECT_FALSE(ruvia::merge_http_304_response_headers(cached, no_validator));
    ruvia::http_response unvalidated;
    EXPECT_TRUE(ruvia::merge_http_304_response_headers(unvalidated, no_validator));
}

TEST(RuviaHttpPublicApi, cache_validators_compare_multiline_coding_stacks_without_reordering) {
    const std::array<ruvia::http_header_view, 3> cached{{
        {"ETag", "\"v1\""},
        {"Content-Encoding", "gzip"},
        {"Content-Encoding", "br"},
    }};
    const std::array<ruvia::http_header_view, 2> same{{
        {"ETag", "\"v1\""},
        {"Content-Encoding", "x-gzip, BR"},
    }};
    const std::array<ruvia::http_header_view, 2> reordered{{
        {"ETag", "\"v1\""},
        {"Content-Encoding", "br, gzip"},
    }};
    EXPECT_EQ(ruvia::match_http_cache_validators(cached, same), ruvia::http_cache_validator_match::strong);
    EXPECT_EQ(ruvia::match_http_cache_validators(cached, reordered), ruvia::http_cache_validator_match::none);
    std::vector<ruvia::http_header> repeated;
    repeated.emplace_back("ETag", "\"v1\"");
    repeated.emplace_back("ETag", "\"v1\"");
    EXPECT_FALSE(ruvia::weaken_http_cache_etag(repeated));
    EXPECT_EQ(repeated[0].value(), "\"v1\"");
    EXPECT_EQ(repeated[1].value(), "\"v1\"");
    EXPECT_EQ(ruvia::match_http_cache_validators(repeated, same), ruvia::http_cache_validator_match::none);
}

namespace {

template <typename vary_type>
concept http_vary_allows_rvalue_field_names = requires(vary_type spec) { std::move(spec).field_names(); };

}  // namespace

TEST(RuviaHttpPublicApi, vary_spec_take_field_names_moves_without_copy_and_resets) {
    static_assert(!http_vary_allows_rvalue_field_names<ruvia::http_vary_spec>);
    auto vary = ruvia::parse_http_vary("Accept-Encoding, User-Agent");
    ASSERT_FALSE(vary.empty());
    const auto* const names_ptr = vary.field_names().data();
    const auto taken = std::move(vary).take_field_names();
    EXPECT_EQ(taken.data(), names_ptr);
    EXPECT_EQ(taken.size(), std::size_t{2});
    EXPECT_TRUE(vary.empty());
}
