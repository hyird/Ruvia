#include "test_harness.h"

#include <string_view>

#include "http/HeaderAcceptUtils.h"

namespace {

using ruvia::detail::httpAcceptsMediaType;
using ruvia::detail::httpMediaRangeMatches;
using ruvia::detail::httpMediaRangeSpecificity;
using ruvia::detail::httpParseQualityValue;
using ruvia::detail::httpQualityParameter;

}  // namespace

RUVIA_TEST(parse_quality_value_rfc7231_grammar) {
    // qvalue = ( "0" [ "." 0*3DIGIT ] ) / ( "1" [ "." 0*3("0") ] ), mapped to
    // milli-units 0..1000.
    RUVIA_CHECK_EQ(httpParseQualityValue("1"), 1000);
    RUVIA_CHECK_EQ(httpParseQualityValue("0"), 0);
    RUVIA_CHECK_EQ(httpParseQualityValue("0.5"), 500);
    RUVIA_CHECK_EQ(httpParseQualityValue("0.500"), 500);
    RUVIA_CHECK_EQ(httpParseQualityValue("0.123"), 123);
    RUVIA_CHECK_EQ(httpParseQualityValue("0.999"), 999);
    RUVIA_CHECK_EQ(httpParseQualityValue("1.0"), 1000);
    RUVIA_CHECK_EQ(httpParseQualityValue("1.000"), 1000);

    // Invalid qvalues yield -1: greater than 1, a non-zero fraction on 1.x, more
    // than three fraction digits, an out-of-range integer, and non-numeric input.
    RUVIA_CHECK_EQ(httpParseQualityValue("1.5"), -1);
    RUVIA_CHECK_EQ(httpParseQualityValue("1.1"), -1);
    RUVIA_CHECK_EQ(httpParseQualityValue("0.1234"), -1);
    RUVIA_CHECK_EQ(httpParseQualityValue("2"), -1);
    RUVIA_CHECK_EQ(httpParseQualityValue("abc"), -1);
    RUVIA_CHECK_EQ(httpParseQualityValue(""), -1);
}

RUVIA_TEST(quality_parameter_extracts_q_from_header_item) {
    // A header item without a q parameter defaults to full quality (1000): the
    // media range / coding is present and acceptable.
    RUVIA_CHECK_EQ(httpQualityParameter("gzip"), 1000);
    RUVIA_CHECK_EQ(httpQualityParameter("text/html"), 1000);

    // The q parameter is extracted and its name matched case-insensitively.
    RUVIA_CHECK_EQ(httpQualityParameter("gzip;q=0.5"), 500);
    RUVIA_CHECK_EQ(httpQualityParameter("text/plain;Q=0.8"), 800);

    // A syntactically INVALID q collapses to 0 (not acceptable) rather than the
    // present-default of 1000 -- a malformed q must never be read as "accept".
    RUVIA_CHECK_EQ(httpQualityParameter("gzip;q=2"), 0);
    RUVIA_CHECK_EQ(httpQualityParameter("gzip;q=bogus"), 0);

    // The first q wins; parameters after it are accept-ext and do not override it.
    RUVIA_CHECK_EQ(httpQualityParameter("gzip;q=0.3;q=0.9"), 300);

    // A ';' inside a quoted parameter value is not a parameter separator, so a
    // fake q smuggled inside quotes is ignored and the real trailing q is used.
    RUVIA_CHECK_EQ(httpQualityParameter(R"(a;note="x;q=1";q=0.4)"), 400);
}

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

RUVIA_TEST(media_range_rejects_invalid_tokens) {
    RUVIA_CHECK_EQ(httpMediaRangeSpecificity("text/"), -1);
    RUVIA_CHECK_EQ(httpMediaRangeSpecificity("/json"), -1);
    RUVIA_CHECK_EQ(httpMediaRangeSpecificity("*/json"), -1);
    RUVIA_CHECK_EQ(httpMediaRangeSpecificity("text /html"), -1);

    RUVIA_CHECK(!httpMediaRangeMatches("text/*", "text/"));
    RUVIA_CHECK(!httpAcceptsMediaType("*/json, */*;q=0", "application/json"));
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
