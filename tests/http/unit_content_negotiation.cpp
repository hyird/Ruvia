#include "test_harness.h"

#include <string_view>

#include "ruvia/http/detail/HeaderAcceptUtils.h"

namespace {

using ruvia::detail::HttpAcceptedEncodingQuality;
using ruvia::detail::HttpContentCoding;
using ruvia::detail::HttpResponseCodingQualities;

// Reference form: one full Accept-Encoding scan per coding. The aggregate
// single-pass update must produce identical qualities.
struct ReferenceQualities final {
    HttpAcceptedEncodingQuality gzip;
    HttpAcceptedEncodingQuality brotli;
    HttpAcceptedEncodingQuality zstd;
    HttpAcceptedEncodingQuality identity;
};

ReferenceQualities referenceThreePass(std::string_view header) {
    ReferenceQualities ref;
    ref.gzip.update(header, "gzip");
    ref.brotli.update(header, "br");
    ref.zstd.update(header, "zstd");
    ref.identity.update(header, "identity");
    return ref;
}

bool sameQuality(const HttpAcceptedEncodingQuality& a, const HttpAcceptedEncodingQuality& b) {
    return a.explicitQuality == b.explicitQuality && a.wildcardQuality == b.wildcardQuality;
}

}  // namespace

RUVIA_TEST(response_coding_single_pass_matches_per_coding_scans) {
    const std::string_view cases[] = {
        "gzip, br, zstd",
        "gzip;q=0.5, br;q=0.8, zstd;q=0.3",
        "*",
        "*;q=0.1, gzip;q=0.9",
        "br;q=0, *;q=0.5",
        "identity, gzip",
        "  gzip ,  br ",
        "GZIP, Br, ZSTD",  // token match is case-insensitive
        "",
        "deflate;q=0.2",   // unknown coding: leaves all three untouched
        "gzip;q=0, gzip;q=0.9",
        ", gzip, , br,",         // empty items are skipped
        "gzip;q=1.0, br;q=0.500",
        R"(gzip;note="a,b";q=0, br;q=0.5)",
    };
    for (const auto header : cases) {
        HttpResponseCodingQualities qualities;
        qualities.update(header);

        const auto ref = referenceThreePass(header);
        RUVIA_CHECK(sameQuality(qualities.gzip, ref.gzip));
        RUVIA_CHECK(sameQuality(qualities.brotli, ref.brotli));
        RUVIA_CHECK(sameQuality(qualities.zstd, ref.zstd));
        RUVIA_CHECK(sameQuality(qualities.identity, ref.identity));
    }
}

RUVIA_TEST(response_coding_selection_end_to_end) {
    using ruvia::detail::httpSelectResponseCoding;
    // Server tie-break prefers br > zstd > gzip at equal q.
    RUVIA_CHECK(httpSelectResponseCoding("gzip, br, zstd") == HttpContentCoding::kBrotli);
    // Explicit q ordering wins over the tie-break.
    RUVIA_CHECK(httpSelectResponseCoding("identity;q=0, gzip;q=0.9, br;q=0.1") == HttpContentCoding::kGzip);
    // A coding at q=0 is excluded even under a permissive wildcard.
    RUVIA_CHECK(httpSelectResponseCoding(
        "identity;q=0, br;q=0, *;q=0.5") == HttpContentCoding::kZstd);
    // identity is implicitly q=1, so a lower-quality coding must not override it.
    RUVIA_CHECK(httpSelectResponseCoding("gzip;q=0.9") == HttpContentCoding::kIdentity);
    // A positive wildcard does not lower identity's implicit quality, while an
    // explicit identity preference does.
    RUVIA_CHECK(httpSelectResponseCoding("*;q=0.5") == HttpContentCoding::kIdentity);
    RUVIA_CHECK(httpSelectResponseCoding("identity;q=0.1, gzip;q=0.5") == HttpContentCoding::kGzip);
    // Repeating the same coding is equivalent to multiple matching alternatives:
    // the highest qvalue wins, independently of list order.
    RUVIA_CHECK(httpSelectResponseCoding(
        "identity;q=0.5, gzip;q=0.9, gzip;q=0.1") == HttpContentCoding::kGzip);
    RUVIA_CHECK(httpSelectResponseCoding(
        "identity;q=0.5, gzip;q=0.1, gzip;q=0.9") == HttpContentCoding::kGzip);
    // The same rule applies to repeated wildcard entries for an unlisted coding.
    RUVIA_CHECK(httpSelectResponseCoding(
        "identity;q=0, *;q=0.8, *;q=0.1") == HttpContentCoding::kBrotli);
    HttpResponseCodingQualities repeated;
    repeated.update("gzip;q=0.9, gzip;q=0.1, *;q=0.8, *;q=0.2");
    RUVIA_CHECK_EQ(repeated.gzip.explicitQuality, 900);
    RUVIA_CHECK_EQ(repeated.brotli.wildcardQuality, 800);
    HttpResponseCodingQualities splitLines;
    splitLines.update("gzip;q=0.9, *;q=0.8");
    splitLines.update("gzip;q=0.1, *;q=0.2");
    RUVIA_CHECK_EQ(splitLines.gzip.explicitQuality, 900);
    RUVIA_CHECK_EQ(splitLines.brotli.wildcardQuality, 800);
    // Accept-Encoding allows only an optional weight after a coding. Unknown
    // parameters and whitespace around q's '=' make the item invalid; they must
    // not inherit the default q=1 and outrank identity.
    RUVIA_CHECK(httpSelectResponseCoding(
        "identity;q=0.5, gzip;level=9") == HttpContentCoding::kIdentity);
    RUVIA_CHECK(httpSelectResponseCoding(
        "identity;q=0.5, gzip;q =1") == HttpContentCoding::kIdentity);
    RUVIA_CHECK(httpSelectResponseCoding(
        "identity;q=0.5, gzip;q= 1") == HttpContentCoding::kIdentity);
    // OWS around the weight delimiter itself is explicitly allowed.
    RUVIA_CHECK(httpSelectResponseCoding(
        "identity;q=0.5, gzip \t; \tq=0.8") == HttpContentCoding::kGzip);
    // No acceptable coding.
    RUVIA_CHECK(httpSelectResponseCoding("identity") == HttpContentCoding::kIdentity);
    RUVIA_CHECK(httpSelectResponseCoding("") == HttpContentCoding::kIdentity);
}
