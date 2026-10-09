#include <cstdint>
#include <limits>

#include "ruvia/http/http_cache.h"

#include "test_harness.h"

RUVIA_TEST(parse_cache_control_flags_and_ages) {
    const auto cc = ruvia::parse_cache_control(
        "public, max-age=60, s-maxage=120, stale-while-revalidate=30, immutable");
    RUVIA_CHECK(cc.has(ruvia::cache_control_directive::public_value));
    RUVIA_CHECK(!cc.has(ruvia::cache_control_directive::no_store));
    RUVIA_CHECK(cc.has(ruvia::cache_control_directive::immutable));
    RUVIA_CHECK(!cc.has(static_cast<ruvia::cache_control_directive>(
        static_cast<std::uint16_t>(ruvia::cache_control_directive::public_value) |
        static_cast<std::uint16_t>(ruvia::cache_control_directive::immutable))));
    RUVIA_CHECK(cc.max_age().has_value());
    RUVIA_CHECK_EQ(*cc.max_age(), std::uint64_t{60});
    RUVIA_CHECK_EQ(*cc.s_max_age(), std::uint64_t{120});
    RUVIA_CHECK_EQ(*cc.stale_while_revalidate(), std::uint64_t{30});
}

RUVIA_TEST(parse_cache_control_request_directives) {
    const auto request =
        ruvia::parse_cache_control("only-if-cached, max-age=0, min-fresh=15, max-stale=30");
    RUVIA_CHECK(request.has(ruvia::cache_control_directive::only_if_cached));
    RUVIA_CHECK_EQ(request.max_age().value_or(1), std::uint64_t{0});
    RUVIA_CHECK_EQ(request.min_fresh().value_or(0), std::uint64_t{15});
    RUVIA_CHECK_EQ(request.max_stale().value_or(0), std::uint64_t{30});
    RUVIA_CHECK(!request.has(ruvia::cache_control_directive::max_stale_any));

    const auto any_stale = ruvia::parse_cache_control("max-stale");
    RUVIA_CHECK(any_stale.has(ruvia::cache_control_directive::max_stale_any));
    RUVIA_CHECK(!any_stale.max_stale().has_value());

    const auto invalid =
        ruvia::parse_cache_control("only-if-cached=yes, min-fresh=bad, max-stale=bad");
    RUVIA_CHECK(!invalid.has(ruvia::cache_control_directive::only_if_cached));
    RUVIA_CHECK(!invalid.min_fresh().has_value());
    RUVIA_CHECK(!invalid.max_stale().has_value());
    RUVIA_CHECK(!invalid.has(ruvia::cache_control_directive::max_stale_any));
}

RUVIA_TEST(parse_cache_control_request_freshness_uses_first_occurrence) {
    ruvia::cache_control_field_parser parser;
    parser.update("min-fresh=5, max-stale=10");
    parser.update("min-fresh=50, max-stale");
    const auto request = parser.finish();
    RUVIA_CHECK_EQ(request.min_fresh().value_or(0), std::uint64_t{5});
    RUVIA_CHECK_EQ(request.max_stale().value_or(0), std::uint64_t{10});
    RUVIA_CHECK(!request.has(ruvia::cache_control_directive::max_stale_any));
}

RUVIA_TEST(parse_cache_control_no_store_and_private) {
    const auto cc = ruvia::parse_cache_control("no-store, private, must-revalidate");
    RUVIA_CHECK(cc.has(ruvia::cache_control_directive::no_store));
    RUVIA_CHECK(cc.has(ruvia::cache_control_directive::private_value));
    RUVIA_CHECK(cc.has(ruvia::cache_control_directive::must_revalidate));
    RUVIA_CHECK(!cc.max_age().has_value());
}

RUVIA_TEST(parse_cache_control_no_transform_is_bare_and_quote_aware) {
    const auto present = ruvia::parse_cache_control("NO-TRANSFORM");
    RUVIA_CHECK(present.has(ruvia::cache_control_directive::no_transform));

    const auto quoted = ruvia::parse_cache_control(R"(extension="a, no-transform, b")");
    RUVIA_CHECK(!quoted.has(ruvia::cache_control_directive::no_transform));

    const auto qualified = ruvia::parse_cache_control("no-transform=ignored");
    RUVIA_CHECK(!qualified.has(ruvia::cache_control_directive::no_transform));
}

RUVIA_TEST(parse_cache_control_quoted_and_case_insensitive_and_unknown) {
    // Directive names are case-insensitive; a quoted delta-seconds is accepted; unknown ignored.
    const auto cc = ruvia::parse_cache_control("Max-Age=\"45\" , surrogate-control=foo, No-Cache");
    RUVIA_CHECK(cc.has(ruvia::cache_control_directive::no_cache));
    RUVIA_CHECK(cc.max_age().has_value());
    RUVIA_CHECK_EQ(*cc.max_age(), std::uint64_t{45});
}

RUVIA_TEST(parse_cache_control_decodes_quoted_pairs_in_delta_seconds) {
    const auto cc = ruvia::parse_cache_control(
        R"(max-age="6\0", s-maxage="1\20", stale-while-revalidate="\30", stale-if-error="4\5")");
    RUVIA_CHECK_EQ(cc.max_age().value_or(0), std::uint64_t{60});
    RUVIA_CHECK_EQ(cc.s_max_age().value_or(0), std::uint64_t{120});
    RUVIA_CHECK_EQ(cc.stale_while_revalidate().value_or(0), std::uint64_t{30});
    RUVIA_CHECK_EQ(cc.stale_if_error().value_or(0), std::uint64_t{45});
}

RUVIA_TEST(parse_cache_control_rejects_bad_delta_seconds) {
    const auto cc = ruvia::parse_cache_control("max-age=abc, s-maxage=");
    RUVIA_CHECK(!cc.max_age().has_value());
    RUVIA_CHECK(!cc.s_max_age().has_value());
}

RUVIA_TEST(parse_cache_control_rejects_whitespace_around_equals) {
    const auto cc = ruvia::parse_cache_control(
        "max-age =60, s-maxage= 120, stale-while-revalidate = 30, "
        "stale-if-error=\t45, no-cache =\"Set-Cookie\", private = auth, "
        "no-cache= \"ETag\", private= auth");
    RUVIA_CHECK(!cc.max_age().has_value());
    RUVIA_CHECK(!cc.s_max_age().has_value());
    RUVIA_CHECK(!cc.stale_while_revalidate().has_value());
    RUVIA_CHECK(!cc.stale_if_error().has_value());
    RUVIA_CHECK(!cc.has(ruvia::cache_control_directive::no_cache));
    RUVIA_CHECK(!cc.has(ruvia::cache_control_directive::private_value));

    // OWS around comma separators remains valid list framing.
    const auto separated = ruvia::parse_cache_control(" max-age=60 \t,\t no-store ");
    RUVIA_CHECK_EQ(separated.max_age().value_or(0), std::uint64_t{60});
    RUVIA_CHECK(separated.has(ruvia::cache_control_directive::no_store));
}

RUVIA_TEST(parse_cache_control_freshness_uses_first_occurrence) {
    const auto duplicated = ruvia::parse_cache_control(
        "max-age=60, MAX-AGE=3600, s-maxage=120, s-maxage=7200, "
        "stale-while-revalidate=30, stale-while-revalidate=300, "
        "stale-if-error=45, stale-if-error=450");
    RUVIA_CHECK_EQ(duplicated.max_age().value_or(0), std::uint64_t{60});
    RUVIA_CHECK_EQ(duplicated.s_max_age().value_or(0), std::uint64_t{120});
    RUVIA_CHECK_EQ(duplicated.stale_while_revalidate().value_or(0), std::uint64_t{30});
    RUVIA_CHECK_EQ(duplicated.stale_if_error().value_or(0), std::uint64_t{45});

    // An invalid first occurrence cannot be repaired by a later value; caches
    // must not accidentally turn invalid freshness information into freshness.
    const auto invalid_first =
        ruvia::parse_cache_control("max-age=invalid, max-age=3600, s-maxage=, s-maxage=7200");
    RUVIA_CHECK(!invalid_first.max_age().has_value());
    RUVIA_CHECK(!invalid_first.s_max_age().has_value());
}

RUVIA_TEST(cache_control_field_parser_combines_repeated_lines) {
    ruvia::cache_control_field_parser parser;
    parser.update("public, max-age=invalid, s-maxage=120");
    parser.update(R"(extension="a, no-transform, b", no-transform, max-age=3600, s-maxage=7200)");

    const auto cc = parser.finish();
    RUVIA_CHECK(cc.has(ruvia::cache_control_directive::public_value));
    RUVIA_CHECK(cc.has(ruvia::cache_control_directive::no_transform));
    RUVIA_CHECK(!cc.max_age().has_value());
    RUVIA_CHECK_EQ(cc.s_max_age().value_or(0), std::uint64_t{120});
}

RUVIA_TEST(parse_cache_control_delta_seconds_overflow_saturates) {
    const auto cc = ruvia::parse_cache_control(
        "max-age=184467440737095516150, "
        "s-maxage=\"184467440737095516150\"");
    RUVIA_CHECK_EQ(cc.max_age().value_or(0), (std::numeric_limits<std::uint64_t>::max)());
    RUVIA_CHECK_EQ(cc.s_max_age().value_or(0), (std::numeric_limits<std::uint64_t>::max)());
}

RUVIA_TEST(parse_cache_control_does_not_split_quoted_extension_values) {
    const auto cc = ruvia::parse_cache_control("extension=\"a, public, max-age=999, b\", private");
    RUVIA_CHECK(!cc.has(ruvia::cache_control_directive::public_value));
    RUVIA_CHECK(!cc.max_age().has_value());
    RUVIA_CHECK(cc.has(ruvia::cache_control_directive::private_value));

    // A quoted-pair keeps the escaped quote inside the extension value; its
    // commas likewise cannot introduce directives.
    const auto escaped = ruvia::parse_cache_control("extension=\"a\\\", no-store, b\", immutable");
    RUVIA_CHECK(!escaped.has(ruvia::cache_control_directive::no_store));
    RUVIA_CHECK(escaped.has(ruvia::cache_control_directive::immutable));

    // An unterminated quoted value is malformed through the end of the field,
    // so a comma within it must not accidentally enable caching directives.
    const auto unterminated = ruvia::parse_cache_control("extension=\"a, public, max-age=3600");
    RUVIA_CHECK(!unterminated.has(ruvia::cache_control_directive::public_value));
    RUVIA_CHECK(!unterminated.max_age().has_value());
}

RUVIA_TEST(parse_cache_control_ignores_arguments_on_bare_directives) {
    const auto cc = ruvia::parse_cache_control(
        "public=ignored, no-store=ignored, must-revalidate=x, "
        "proxy-revalidate=\"x\", no-transform=ignored, immutable=");
    RUVIA_CHECK(!cc.has(ruvia::cache_control_directive::public_value));
    RUVIA_CHECK(!cc.has(ruvia::cache_control_directive::no_store));
    RUVIA_CHECK(!cc.has(ruvia::cache_control_directive::must_revalidate));
    RUVIA_CHECK(!cc.has(ruvia::cache_control_directive::proxy_revalidate));
    RUVIA_CHECK(!cc.has(ruvia::cache_control_directive::no_transform));
    RUVIA_CHECK(!cc.has(ruvia::cache_control_directive::immutable));

    // no-cache and private are different: their response forms can carry a
    // field-name list, so an argument does not invalidate the directive.
    const auto qualified =
        ruvia::parse_cache_control("no-cache=\"Set-Cookie\", private=\"Authorization\"");
    RUVIA_CHECK(qualified.has(ruvia::cache_control_directive::no_cache));
    RUVIA_CHECK(qualified.has(ruvia::cache_control_directive::private_value));
}

RUVIA_TEST(parse_http_date_imf_fixdate) {
    // RFC 7231 example: Sun, 06 Nov 1994 08:49:37 GMT.
    const auto t = ruvia::parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT");
    RUVIA_CHECK(t.has_value());
    RUVIA_CHECK_EQ(static_cast<long long>(*t), 784111777LL);
    RUVIA_CHECK(!ruvia::parse_http_date("not a date").has_value());
}
