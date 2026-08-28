#include "test_harness.h"

#include <cstdint>
#include <limits>

#include "ruvia/http/HttpCache.h"

RUVIA_TEST(parse_cache_control_flags_and_ages) {
    const auto cc = ruvia::parseCacheControl("public, max-age=60, s-maxage=120, stale-while-revalidate=30, immutable");
    RUVIA_CHECK(cc.has(ruvia::CacheControlDirective::kPublic));
    RUVIA_CHECK(!cc.has(ruvia::CacheControlDirective::kNoStore));
    RUVIA_CHECK(cc.has(ruvia::CacheControlDirective::kImmutable));
    RUVIA_CHECK(!cc.has(static_cast<ruvia::CacheControlDirective>(static_cast<std::uint16_t>(ruvia::CacheControlDirective::kPublic) | static_cast<std::uint16_t>(ruvia::CacheControlDirective::kImmutable))));
    RUVIA_CHECK(cc.maxAge().has_value());
    RUVIA_CHECK_EQ(*cc.maxAge(), std::uint64_t{60});
    RUVIA_CHECK_EQ(*cc.sMaxAge(), std::uint64_t{120});
    RUVIA_CHECK_EQ(*cc.staleWhileRevalidate(), std::uint64_t{30});
}

RUVIA_TEST(parse_cache_control_request_directives) {
    const auto request = ruvia::parseCacheControl("only-if-cached, max-age=0, min-fresh=15, max-stale=30");
    RUVIA_CHECK(request.has(ruvia::CacheControlDirective::kOnlyIfCached));
    RUVIA_CHECK_EQ(request.maxAge().value_or(1), std::uint64_t{0});
    RUVIA_CHECK_EQ(request.minFresh().value_or(0), std::uint64_t{15});
    RUVIA_CHECK_EQ(request.maxStale().value_or(0), std::uint64_t{30});
    RUVIA_CHECK(!request.has(ruvia::CacheControlDirective::kMaxStaleAny));

    const auto anyStale = ruvia::parseCacheControl("max-stale");
    RUVIA_CHECK(anyStale.has(ruvia::CacheControlDirective::kMaxStaleAny));
    RUVIA_CHECK(!anyStale.maxStale().has_value());

    const auto invalid = ruvia::parseCacheControl("only-if-cached=yes, min-fresh=bad, max-stale=bad");
    RUVIA_CHECK(!invalid.has(ruvia::CacheControlDirective::kOnlyIfCached));
    RUVIA_CHECK(!invalid.minFresh().has_value());
    RUVIA_CHECK(!invalid.maxStale().has_value());
    RUVIA_CHECK(!invalid.has(ruvia::CacheControlDirective::kMaxStaleAny));
}

RUVIA_TEST(parse_cache_control_request_freshness_uses_first_occurrence) {
    ruvia::CacheControlFieldParser parser;
    parser.update("min-fresh=5, max-stale=10");
    parser.update("min-fresh=50, max-stale");
    const auto request = parser.finish();
    RUVIA_CHECK_EQ(request.minFresh().value_or(0), std::uint64_t{5});
    RUVIA_CHECK_EQ(request.maxStale().value_or(0), std::uint64_t{10});
    RUVIA_CHECK(!request.has(ruvia::CacheControlDirective::kMaxStaleAny));
}

RUVIA_TEST(parse_cache_control_no_store_and_private) {
    const auto cc = ruvia::parseCacheControl("no-store, private, must-revalidate");
    RUVIA_CHECK(cc.has(ruvia::CacheControlDirective::kNoStore));
    RUVIA_CHECK(cc.has(ruvia::CacheControlDirective::kPrivate));
    RUVIA_CHECK(cc.has(ruvia::CacheControlDirective::kMustRevalidate));
    RUVIA_CHECK(!cc.maxAge().has_value());
}

RUVIA_TEST(parse_cache_control_no_transform_is_bare_and_quote_aware) {
    const auto present = ruvia::parseCacheControl("NO-TRANSFORM");
    RUVIA_CHECK(present.has(ruvia::CacheControlDirective::kNoTransform));

    const auto quoted = ruvia::parseCacheControl(R"(extension="a, no-transform, b")");
    RUVIA_CHECK(!quoted.has(ruvia::CacheControlDirective::kNoTransform));

    const auto qualified = ruvia::parseCacheControl("no-transform=ignored");
    RUVIA_CHECK(!qualified.has(ruvia::CacheControlDirective::kNoTransform));
}

RUVIA_TEST(parse_cache_control_quoted_and_case_insensitive_and_unknown) {
    // Directive names are case-insensitive; a quoted delta-seconds is accepted; unknown ignored.
    const auto cc = ruvia::parseCacheControl("Max-Age=\"45\" , surrogate-control=foo, No-Cache");
    RUVIA_CHECK(cc.has(ruvia::CacheControlDirective::kNoCache));
    RUVIA_CHECK(cc.maxAge().has_value());
    RUVIA_CHECK_EQ(*cc.maxAge(), std::uint64_t{45});
}

RUVIA_TEST(parse_cache_control_decodes_quoted_pairs_in_delta_seconds) {
    const auto cc = ruvia::parseCacheControl(R"(max-age="6\0", s-maxage="1\20", stale-while-revalidate="\30", stale-if-error="4\5")");
    RUVIA_CHECK_EQ(cc.maxAge().value_or(0), std::uint64_t{60});
    RUVIA_CHECK_EQ(cc.sMaxAge().value_or(0), std::uint64_t{120});
    RUVIA_CHECK_EQ(cc.staleWhileRevalidate().value_or(0), std::uint64_t{30});
    RUVIA_CHECK_EQ(cc.staleIfError().value_or(0), std::uint64_t{45});
}

RUVIA_TEST(parse_cache_control_rejects_bad_delta_seconds) {
    const auto cc = ruvia::parseCacheControl("max-age=abc, s-maxage=");
    RUVIA_CHECK(!cc.maxAge().has_value());
    RUVIA_CHECK(!cc.sMaxAge().has_value());
}

RUVIA_TEST(parse_cache_control_rejects_whitespace_around_equals) {
    const auto cc = ruvia::parseCacheControl(
        "max-age =60, s-maxage= 120, stale-while-revalidate = 30, "
        "stale-if-error=\t45, no-cache =\"Set-Cookie\", private = auth, "
        "no-cache= \"ETag\", private= auth");
    RUVIA_CHECK(!cc.maxAge().has_value());
    RUVIA_CHECK(!cc.sMaxAge().has_value());
    RUVIA_CHECK(!cc.staleWhileRevalidate().has_value());
    RUVIA_CHECK(!cc.staleIfError().has_value());
    RUVIA_CHECK(!cc.has(ruvia::CacheControlDirective::kNoCache));
    RUVIA_CHECK(!cc.has(ruvia::CacheControlDirective::kPrivate));

    // OWS around comma separators remains valid list framing.
    const auto separated = ruvia::parseCacheControl(" max-age=60 \t,\t no-store ");
    RUVIA_CHECK_EQ(separated.maxAge().value_or(0), std::uint64_t{60});
    RUVIA_CHECK(separated.has(ruvia::CacheControlDirective::kNoStore));
}

RUVIA_TEST(parse_cache_control_freshness_uses_first_occurrence) {
    const auto duplicated = ruvia::parseCacheControl(
        "max-age=60, MAX-AGE=3600, s-maxage=120, s-maxage=7200, "
        "stale-while-revalidate=30, stale-while-revalidate=300, "
        "stale-if-error=45, stale-if-error=450");
    RUVIA_CHECK_EQ(duplicated.maxAge().value_or(0), std::uint64_t{60});
    RUVIA_CHECK_EQ(duplicated.sMaxAge().value_or(0), std::uint64_t{120});
    RUVIA_CHECK_EQ(duplicated.staleWhileRevalidate().value_or(0), std::uint64_t{30});
    RUVIA_CHECK_EQ(duplicated.staleIfError().value_or(0), std::uint64_t{45});

    // An invalid first occurrence cannot be repaired by a later value; caches
    // must not accidentally turn invalid freshness information into freshness.
    const auto invalidFirst = ruvia::parseCacheControl("max-age=invalid, max-age=3600, s-maxage=, s-maxage=7200");
    RUVIA_CHECK(!invalidFirst.maxAge().has_value());
    RUVIA_CHECK(!invalidFirst.sMaxAge().has_value());
}

RUVIA_TEST(cache_control_field_parser_combines_repeated_lines) {
    ruvia::CacheControlFieldParser parser;
    parser.update("public, max-age=invalid, s-maxage=120");
    parser.update(R"(extension="a, no-transform, b", no-transform, max-age=3600, s-maxage=7200)");

    const auto cc = parser.finish();
    RUVIA_CHECK(cc.has(ruvia::CacheControlDirective::kPublic));
    RUVIA_CHECK(cc.has(ruvia::CacheControlDirective::kNoTransform));
    RUVIA_CHECK(!cc.maxAge().has_value());
    RUVIA_CHECK_EQ(cc.sMaxAge().value_or(0), std::uint64_t{120});
}

RUVIA_TEST(parse_cache_control_delta_seconds_overflow_saturates) {
    const auto cc = ruvia::parseCacheControl(
        "max-age=184467440737095516150, "
        "s-maxage=\"184467440737095516150\"");
    RUVIA_CHECK_EQ(cc.maxAge().value_or(0), (std::numeric_limits<std::uint64_t>::max)());
    RUVIA_CHECK_EQ(cc.sMaxAge().value_or(0), (std::numeric_limits<std::uint64_t>::max)());
}

RUVIA_TEST(parse_cache_control_does_not_split_quoted_extension_values) {
    const auto cc = ruvia::parseCacheControl("extension=\"a, public, max-age=999, b\", private");
    RUVIA_CHECK(!cc.has(ruvia::CacheControlDirective::kPublic));
    RUVIA_CHECK(!cc.maxAge().has_value());
    RUVIA_CHECK(cc.has(ruvia::CacheControlDirective::kPrivate));

    // A quoted-pair keeps the escaped quote inside the extension value; its
    // commas likewise cannot introduce directives.
    const auto escaped = ruvia::parseCacheControl("extension=\"a\\\", no-store, b\", immutable");
    RUVIA_CHECK(!escaped.has(ruvia::CacheControlDirective::kNoStore));
    RUVIA_CHECK(escaped.has(ruvia::CacheControlDirective::kImmutable));

    // An unterminated quoted value is malformed through the end of the field,
    // so a comma within it must not accidentally enable caching directives.
    const auto unterminated = ruvia::parseCacheControl("extension=\"a, public, max-age=3600");
    RUVIA_CHECK(!unterminated.has(ruvia::CacheControlDirective::kPublic));
    RUVIA_CHECK(!unterminated.maxAge().has_value());
}

RUVIA_TEST(parse_cache_control_ignores_arguments_on_bare_directives) {
    const auto cc = ruvia::parseCacheControl(
        "public=ignored, no-store=ignored, must-revalidate=x, "
        "proxy-revalidate=\"x\", no-transform=ignored, immutable=");
    RUVIA_CHECK(!cc.has(ruvia::CacheControlDirective::kPublic));
    RUVIA_CHECK(!cc.has(ruvia::CacheControlDirective::kNoStore));
    RUVIA_CHECK(!cc.has(ruvia::CacheControlDirective::kMustRevalidate));
    RUVIA_CHECK(!cc.has(ruvia::CacheControlDirective::kProxyRevalidate));
    RUVIA_CHECK(!cc.has(ruvia::CacheControlDirective::kNoTransform));
    RUVIA_CHECK(!cc.has(ruvia::CacheControlDirective::kImmutable));

    // no-cache and private are different: their response forms can carry a
    // field-name list, so an argument does not invalidate the directive.
    const auto qualified = ruvia::parseCacheControl("no-cache=\"Set-Cookie\", private=\"Authorization\"");
    RUVIA_CHECK(qualified.has(ruvia::CacheControlDirective::kNoCache));
    RUVIA_CHECK(qualified.has(ruvia::CacheControlDirective::kPrivate));
}

RUVIA_TEST(parse_http_date_imf_fixdate) {
    // RFC 7231 example: Sun, 06 Nov 1994 08:49:37 GMT.
    const auto t = ruvia::parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT");
    RUVIA_CHECK(t.has_value());
    RUVIA_CHECK_EQ(static_cast<long long>(*t), 784111777LL);
    RUVIA_CHECK(!ruvia::parseHttpDate("not a date").has_value());
}
