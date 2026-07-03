#include "test_harness.h"

#include <ctime>
#include <string_view>

#include "http/FileResponseHelpers.h"

// ETag comparison and IMF-fixdate parsing back the conditional-request handling
// (If-Match / If-None-Match / If-Range, RFC 7232) for static file responses.

RUVIA_TEST(etag_strong_comparison) {
    using ruvia::detail::httpStrongEtagEquals;
    // Strong compare: both must be strong (no "W/") and octet-equal.
    RUVIA_CHECK(httpStrongEtagEquals(R"("abc")", R"("abc")"));
    RUVIA_CHECK(!httpStrongEtagEquals(R"("abc")", R"("abd")"));       // different tag
    RUVIA_CHECK(!httpStrongEtagEquals(R"(W/"abc")", R"("abc")"));     // one weak -> never strong-equal
    RUVIA_CHECK(!httpStrongEtagEquals(R"(W/"abc")", R"(W/"abc")"));   // both weak -> not a strong match
}

RUVIA_TEST(etag_weak_comparison) {
    using ruvia::detail::httpWeakEtagEquals;
    // Weak compare: equal after stripping an optional "W/" prefix.
    RUVIA_CHECK(httpWeakEtagEquals(R"(W/"abc")", R"("abc")"));
    RUVIA_CHECK(httpWeakEtagEquals(R"(W/"abc")", R"(W/"abc")"));
    RUVIA_CHECK(httpWeakEtagEquals(R"("abc")", R"("abc")"));
    RUVIA_CHECK(!httpWeakEtagEquals(R"(W/"abc")", R"(W/"abd")"));     // different opaque tags
}

RUVIA_TEST(etag_weak_prefix_detection) {
    using ruvia::detail::httpIsWeakEtag;
    RUVIA_CHECK(httpIsWeakEtag(R"(W/"abc")"));
    RUVIA_CHECK(!httpIsWeakEtag(R"("abc")"));
    RUVIA_CHECK(!httpIsWeakEtag("W"));  // too short to be the "W/" marker
}

RUVIA_TEST(imf_fixdate_parses_known_dates) {
    using ruvia::detail::httpParseImfFixdate;
    const auto epoch = httpParseImfFixdate("Thu, 01 Jan 1970 00:00:00 GMT");
    RUVIA_CHECK(epoch.has_value());
    if (epoch) {
        RUVIA_CHECK_EQ(*epoch, std::time_t{0});
    }
    const auto nextDay = httpParseImfFixdate("Fri, 02 Jan 1970 00:00:00 GMT");
    RUVIA_CHECK(nextDay.has_value());
    if (nextDay) {
        RUVIA_CHECK_EQ(*nextDay, std::time_t{86400});
    }
    // A later date must compare strictly greater (monotonic).
    const auto later = httpParseImfFixdate("Sun, 06 Nov 1994 08:49:37 GMT");
    RUVIA_CHECK(later.has_value());
    if (later && epoch) {
        RUVIA_CHECK(*later > *epoch);
    }
    // Parsing is deterministic.
    RUVIA_CHECK(httpParseImfFixdate("Sun, 06 Nov 1994 08:49:37 GMT") == later);
}

RUVIA_TEST(imf_fixdate_rejects_malformed) {
    using ruvia::detail::httpParseImfFixdate;
    RUVIA_CHECK(!httpParseImfFixdate("").has_value());
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 00:00:00").has_value());       // wrong length / no GMT
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 00:00:00 UTC").has_value());   // zone must be GMT
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jon 1970 00:00:00 GMT").has_value());   // bad month
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 25:00:00 GMT").has_value());   // hour > 23
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 00 Jan 1970 00:00:00 GMT").has_value());   // day < 1
    RUVIA_CHECK(!httpParseImfFixdate("Thu; 01 Jan 1970 00:00:00 GMT").has_value());   // wrong separator
}

RUVIA_TEST(imf_fixdate_leap_second_boundary) {
    using ruvia::detail::httpParseImfFixdate;
    RUVIA_CHECK(httpParseImfFixdate("Thu, 01 Jan 1970 23:59:60 GMT").has_value());   // leap second allowed
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 23:59:61 GMT").has_value());  // 61 rejected
}
