#include "test_harness.h"

#include "ruvia/http/HttpCache.h"

RUVIA_TEST(parse_cache_control_flags_and_ages) {
    const auto cc = ruvia::parseCacheControl(
        "public, max-age=60, s-maxage=120, stale-while-revalidate=30, immutable");
    RUVIA_CHECK(cc.isPublic);
    RUVIA_CHECK(!cc.noStore);
    RUVIA_CHECK(cc.immutable);
    RUVIA_CHECK(cc.maxAge.has_value());
    RUVIA_CHECK_EQ(*cc.maxAge, std::uint64_t{60});
    RUVIA_CHECK_EQ(*cc.sMaxAge, std::uint64_t{120});
    RUVIA_CHECK_EQ(*cc.staleWhileRevalidate, std::uint64_t{30});
}

RUVIA_TEST(parse_cache_control_no_store_and_private) {
    const auto cc = ruvia::parseCacheControl("no-store, private, must-revalidate");
    RUVIA_CHECK(cc.noStore);
    RUVIA_CHECK(cc.isPrivate);
    RUVIA_CHECK(cc.mustRevalidate);
    RUVIA_CHECK(!cc.maxAge.has_value());
}

RUVIA_TEST(parse_cache_control_quoted_and_case_insensitive_and_unknown) {
    // Directive names are case-insensitive; a quoted delta-seconds is accepted; unknown ignored.
    const auto cc = ruvia::parseCacheControl("Max-Age=\"45\" , surrogate-control=foo, No-Cache");
    RUVIA_CHECK(cc.noCache);
    RUVIA_CHECK(cc.maxAge.has_value());
    RUVIA_CHECK_EQ(*cc.maxAge, std::uint64_t{45});
}

RUVIA_TEST(parse_cache_control_rejects_bad_delta_seconds) {
    const auto cc = ruvia::parseCacheControl("max-age=abc, s-maxage=");
    RUVIA_CHECK(!cc.maxAge.has_value());
    RUVIA_CHECK(!cc.sMaxAge.has_value());
}

RUVIA_TEST(parse_http_date_imf_fixdate) {
    // RFC 7231 example: Sun, 06 Nov 1994 08:49:37 GMT.
    const auto t = ruvia::parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT");
    RUVIA_CHECK(t.has_value());
    RUVIA_CHECK_EQ(static_cast<long long>(*t), 784111777LL);
    RUVIA_CHECK(!ruvia::parseHttpDate("not a date").has_value());
}
