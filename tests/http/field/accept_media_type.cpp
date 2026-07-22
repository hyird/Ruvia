#include "test_harness.h"

#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/http/detail/field/HttpAcceptMediaType.h"
#include "ruvia/http/detail/field/HttpQualityValue.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/Context.h"
#include "ruvia/core/memory/MemoryPool.h"

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

    // A media parameter name cannot occur more than once; duplicate q is invalid.
    RUVIA_CHECK_EQ(httpQualityParameter("gzip;q=0.3;q=0.9"), 0);

    // A ';' inside a quoted parameter value is not a parameter separator, so a
    // fake q smuggled inside quotes is ignored and the real trailing q is used.
    RUVIA_CHECK_EQ(httpQualityParameter(R"(a;note="x;q=1";q=0.4)"), 400);

    // Parameter grammar never permits whitespace around '='.
    RUVIA_CHECK_EQ(httpQualityParameter("text/html;q =1"), 0);
    RUVIA_CHECK_EQ(httpQualityParameter("text/html;q= 1"), 0);
}

RUVIA_TEST(media_range_matches_type_subtype_and_wildcards) {
    RUVIA_CHECK(httpMediaRangeMatches("text/html", "text/html"));
    RUVIA_CHECK(httpMediaRangeMatches("text/*", "text/html"));
    RUVIA_CHECK(httpMediaRangeMatches("*/*", "application/json"));
    RUVIA_CHECK(httpMediaRangeMatches("TEXT/HTML", "text/html"));                 // case-insensitive
    RUVIA_CHECK(httpMediaRangeMatches(
        "text/html;charset=utf-8", "text/html;charset=\"utf-8\""));
    // Charset names are registered case-insensitively; quoted-string syntax
    // does not change that comparison rule.
    RUVIA_CHECK(httpMediaRangeMatches(
        "text/html;charset=\"UTF-8\"", "text/html;CHARSET=utf-8"));
    RUVIA_CHECK(!httpMediaRangeMatches(
        "text/html;charset=utf-8", "text/html"));
    RUVIA_CHECK(!httpMediaRangeMatches(
        "text/html;charset=utf-8", "text/html;charset=iso-8859-1"));
    // Other parameter values retain their registered case-sensitive semantics.
    RUVIA_CHECK(!httpMediaRangeMatches(
        "application/json;profile=Example", "application/json;profile=example"));
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

RUVIA_TEST(media_range_rejects_whitespace_around_parameter_equals) {
    RUVIA_CHECK(!httpAcceptsMediaType("text/html;q =1", "text/html"));
    RUVIA_CHECK(!httpAcceptsMediaType(
        "text/html;level =1", "text/html;level=1"));
    RUVIA_CHECK(!httpAcceptsMediaType(
        "text/html;charset=utf-8", "text/html;charset =utf-8"));

    // OWS around the semicolon delimiter remains legal.
    RUVIA_CHECK(httpAcceptsMediaType(
        "text/html \t; \tq=0.5", "text/html"));
}

RUVIA_TEST(media_range_rejects_duplicate_parameter_names) {
    RUVIA_CHECK(!httpAcceptsMediaType(
        "text/html;level=1;LEVEL=1", "text/html;level=1"));
    RUVIA_CHECK(!httpAcceptsMediaType(
        "text/html;q=1;Q=0", "text/html"));
    RUVIA_CHECK(!httpAcceptsMediaType(
        "text/html;level=1", "text/html;level=1;LEVEL=2"));
}

RUVIA_TEST(media_range_rejects_invalid_offered_parameters) {
    for (const std::string_view offered : {
             "text/plain; charset",
             "text/plain; charset=",
             "text/plain; charset =utf-8",
             "text/plain; charset=utf-8; CHARSET=latin1",
             "text/plain; charset=\"unterminated"}) {
        RUVIA_CHECK(!httpMediaRangeMatches("*/*", offered));
        RUVIA_CHECK(!httpAcceptsMediaType("*/*", offered));
        RUVIA_CHECK(!httpAcceptsMediaType("", offered));
    }
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

RUVIA_TEST(accepts_media_type_parameters_participate_in_matching_and_precedence) {
    // A parameterized range must not match a representation with a different
    // parameter. The generic q=0 range therefore remains the winning match.
    RUVIA_CHECK(!httpAcceptsMediaType(
        "application/json;profile=v2;q=1, application/json;q=0",
        "application/json;profile=v1"));

    // When the parameter does match, that range is more specific than the bare
    // media type and its quality controls acceptance.
    RUVIA_CHECK(httpAcceptsMediaType(
        "application/json;profile=v1;q=0.7, application/json;q=0",
        "application/json;profile=v1"));
    RUVIA_CHECK(!httpAcceptsMediaType(
        "application/json;profile=v1;q=0, application/json;q=1",
        "application/json;profile=v1"));

    // Media-type parameter names are case-insensitive and quoted token-equivalent
    // values compare after quoted-pair decoding. RFC 9110 removed accept-ext, so
    // parameters after q still constrain the media range.
    RUVIA_CHECK(httpAcceptsMediaType(
        R"(text/plain;FORMAT="flowed";q=0.5;extension=ignored)",
        "text/plain;format=flowed;extension=ignored"));
    RUVIA_CHECK(!httpAcceptsMediaType(
        R"(text/plain;q=0.5;format="flowed")",
        "text/plain"));
    RUVIA_CHECK(httpAcceptsMediaType(
        R"(text/plain;q=0.5;format="flowed")",
        "text/plain;format=flowed"));
}

RUVIA_TEST(context_request_accepts_merges_multiple_accept_field_lines) {
    using ruvia::HttpHeaderView;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto accepts = [](std::vector<std::string_view> acceptLines, std::string_view mediaType) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kAccept);
        for (const auto line : acceptLines) {
            HttpRequestAccess::addHeader(request, HttpHeaderView{"Accept", line}, slot);
        }
        auto context = ContextAccess::make(memory, request);
        return context.req().accepts(mediaType);
    };

    // No Accept header -> the client accepts anything.
    RUVIA_CHECK(accepts({}, "text/html"));
    // A present but empty Accept list is distinct from an absent field: it has
    // no matching media range. Empty members remain harmless when another field
    // line supplies an actual range.
    RUVIA_CHECK(!accepts({""}, "text/html"));
    RUVIA_CHECK(accepts({"", "text/html"}, "text/html"));
    // A single line behaves as before.
    RUVIA_CHECK(accepts({"text/html"}, "text/html"));
    RUVIA_CHECK(!accepts({"text/html"}, "application/json"));

    // RFC 9110 5.3: two Accept lines are equivalent to their comma-join. A type
    // offered only on the SECOND line must be accepted -- previously the stored
    // known-header slot held one line and the other was ignored.
    RUVIA_CHECK(accepts({"text/html", "application/json"}, "application/json"));
    RUVIA_CHECK(accepts({"text/html", "application/json"}, "text/html"));
    RUVIA_CHECK(!accepts({"text/html", "application/json"}, "image/png"));

    // A q=0 exclusion whose range is more specific than an accepting range on
    // another line must win, exactly as if joined "text/*, text/html;q=0" -- which
    // a naive per-line OR would get wrong.
    RUVIA_CHECK(!accepts({"text/*", "text/html;q=0"}, "text/html"));
    RUVIA_CHECK(accepts({"text/*", "text/html;q=0"}, "text/plain"));
}
