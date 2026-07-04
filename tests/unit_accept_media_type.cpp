#include "test_harness.h"

#include <string_view>

#include "http/HeaderAcceptUtils.h"

namespace {

using ruvia::detail::httpAcceptsMediaType;
using ruvia::detail::httpMediaRangeMatches;
using ruvia::detail::httpMediaRangeSpecificity;

}  // namespace

RUVIA_TEST(media_range_matches_type_subtype_and_wildcards) {
    RUVIA_CHECK(httpMediaRangeMatches("text/html", "text/html"));
    RUVIA_CHECK(httpMediaRangeMatches("text/*", "text/html"));
    RUVIA_CHECK(httpMediaRangeMatches("*/*", "application/json"));
    RUVIA_CHECK(httpMediaRangeMatches("TEXT/HTML", "text/html"));                 // case-insensitive
    RUVIA_CHECK(httpMediaRangeMatches("text/html;charset=utf-8", "text/html"));   // params ignored
    RUVIA_CHECK(!httpMediaRangeMatches("text/*", "application/json"));            // type mismatch
    RUVIA_CHECK(!httpMediaRangeMatches("text/plain", "text/html"));              // subtype mismatch
    RUVIA_CHECK(!httpMediaRangeMatches("text", "text/html"));                    // no slash -> invalid
}

RUVIA_TEST(media_range_specificity_ordering) {
    // Ordering drives Accept negotiation: full type/subtype > type/* > */*.
    RUVIA_CHECK_EQ(httpMediaRangeSpecificity("text/html"), 2);
    RUVIA_CHECK_EQ(httpMediaRangeSpecificity("text/*"), 1);
    RUVIA_CHECK_EQ(httpMediaRangeSpecificity("*/*"), 0);
    RUVIA_CHECK_EQ(httpMediaRangeSpecificity("bogus"), -1);  // not a media range
}

RUVIA_TEST(accepts_media_type_basic) {
    RUVIA_CHECK(httpAcceptsMediaType("", "text/html"));                // absent Accept -> accept anything
    RUVIA_CHECK(httpAcceptsMediaType("text/html", "text/html"));
    RUVIA_CHECK(httpAcceptsMediaType("text/*", "text/html"));
    RUVIA_CHECK(httpAcceptsMediaType("*/*", "application/json"));
    RUVIA_CHECK(!httpAcceptsMediaType("text/*", "application/json"));  // type mismatch
    RUVIA_CHECK(!httpAcceptsMediaType("text/html;q=0", "text/html"));  // explicit q=0 rejects
}

RUVIA_TEST(accepts_media_type_specificity_beats_quality) {
    // RFC 7231 5.3.2: the MOST SPECIFIC matching range decides, even when a
    // broader range carries a higher q. A specific text/html;q=0 excludes the
    // type despite text/*;q=1.
    RUVIA_CHECK(!httpAcceptsMediaType("text/*;q=1.0, text/html;q=0", "text/html"));
    // A specific positive q wins over a broader q=0.
    RUVIA_CHECK(httpAcceptsMediaType("*/*;q=0, text/html;q=0.5", "text/html"));
    // A broader q=0.5 does not rescue a specific q=0.
    RUVIA_CHECK(!httpAcceptsMediaType("*/*;q=0.5, text/html;q=0", "text/html"));
    // Highest q among equally-specific matches is taken.
    RUVIA_CHECK(httpAcceptsMediaType("text/plain;q=0.5, text/html;q=0.8", "text/html"));
}
