#include "test_harness.h"

#include <string_view>

#include "ruvia/http/detail/HeaderAcceptUtils.h"

namespace {

using ruvia::detail::HttpAcceptedEncodingQuality;
using ruvia::detail::HttpContentCoding;

// The previous three-pass form: one full Accept-Encoding scan per coding. The
// single-pass httpUpdateResponseCodingQualities must produce identical qualities.
struct ReferenceQualities final {
    HttpAcceptedEncodingQuality gzip;
    HttpAcceptedEncodingQuality brotli;
    HttpAcceptedEncodingQuality zstd;
};

ReferenceQualities referenceThreePass(std::string_view header) {
    ReferenceQualities ref;
    ref.gzip.update(header, "gzip");
    ref.brotli.update(header, "br");
    ref.zstd.update(header, "zstd");
    return ref;
}

bool sameQuality(const HttpAcceptedEncodingQuality& a, const HttpAcceptedEncodingQuality& b) {
    return a.explicitQuality == b.explicitQuality && a.wildcardQuality == b.wildcardQuality;
}

}  // namespace

RUVIA_TEST(response_coding_single_pass_matches_three_pass) {
    using ruvia::detail::httpUpdateResponseCodingQualities;
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
        "gzip;q=0, gzip;q=0.9",  // later matching item overwrites
        ", gzip, , br,",         // empty items are skipped
        "gzip;q=1.0, br;q=0.500",
        R"(gzip;note="a,b";q=0, br;q=0.5)",
    };
    for (const auto header : cases) {
        HttpAcceptedEncodingQuality gzip;
        HttpAcceptedEncodingQuality brotli;
        HttpAcceptedEncodingQuality zstd;
        httpUpdateResponseCodingQualities(header, gzip, brotli, zstd);

        const auto ref = referenceThreePass(header);
        RUVIA_CHECK(sameQuality(gzip, ref.gzip));
        RUVIA_CHECK(sameQuality(brotli, ref.brotli));
        RUVIA_CHECK(sameQuality(zstd, ref.zstd));
    }
}

RUVIA_TEST(response_coding_selection_end_to_end) {
    using ruvia::detail::httpSelectResponseCoding;
    // Server tie-break prefers br > zstd > gzip at equal q.
    RUVIA_CHECK(httpSelectResponseCoding("gzip, br, zstd") == HttpContentCoding::kBrotli);
    // Explicit q ordering wins over the tie-break.
    RUVIA_CHECK(httpSelectResponseCoding("gzip;q=0.9, br;q=0.1") == HttpContentCoding::kGzip);
    // A coding at q=0 is excluded even under a permissive wildcard.
    RUVIA_CHECK(httpSelectResponseCoding("br;q=0, *;q=0.5") == HttpContentCoding::kZstd);
    // No acceptable coding.
    RUVIA_CHECK(httpSelectResponseCoding("identity") == HttpContentCoding::kNone);
    RUVIA_CHECK(httpSelectResponseCoding("") == HttpContentCoding::kNone);
}
