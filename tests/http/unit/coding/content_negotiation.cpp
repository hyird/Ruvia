#include "test_harness.h"

#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/coding/HttpAcceptEncoding.h"
#include "ruvia/http/HttpContentCoding.h"

namespace {

using ruvia::detail::HttpAcceptedEncodingQuality;
using ruvia::HttpContentCoding;
using ruvia::detail::HttpResponseCodingCandidates;
using ruvia::detail::HttpResponseCodingQualities;
using ruvia::detail::HttpResponseCodingSelection;
using ruvia::detail::httpParseQualityValue;

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
        "deflate;q=0.2",  // unknown coding: leaves all three untouched
        "gzip;q=0, gzip;q=0.9",
        ", gzip, , br,",  // empty items are skipped
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

RUVIA_TEST(qvalue_parser_rejects_more_than_three_fraction_digits_for_one) {
    RUVIA_CHECK_EQ(httpParseQualityValue("1"), 1000);
    RUVIA_CHECK_EQ(httpParseQualityValue("1."), 1000);
    RUVIA_CHECK_EQ(httpParseQualityValue("1.000"), 1000);
    RUVIA_CHECK_EQ(httpParseQualityValue("0.123"), 123);
    RUVIA_CHECK_EQ(httpParseQualityValue("1.0000"), -1);
    RUVIA_CHECK_EQ(httpParseQualityValue("1.00000"), -1);

    HttpResponseCodingQualities qualities;
    qualities.update("identity;q=0.5, gzip;q=1.0000");
    const auto result = HttpResponseCodingSelection::select(qualities);
    RUVIA_CHECK(result.selected() != nullptr);
    if (const auto* selected = result.selected()) {
        RUVIA_CHECK(selected->coding() == HttpContentCoding::kIdentity);
    }
}

RUVIA_TEST(response_coding_selection_end_to_end) {
    const auto select = [](std::string_view header) {
        HttpResponseCodingQualities qualities;
        qualities.update(header);
        const auto result = HttpResponseCodingSelection::select(qualities);
        const auto* selected = result.selected();
        if (selected == nullptr) {
            throw std::runtime_error("test expected an acceptable response coding");
        }
        return selected->coding();
    };
    // Server tie-break prefers br > zstd > gzip at equal q.
    RUVIA_CHECK(select("gzip, br, zstd") == HttpContentCoding::kBrotli);
    // Explicit q ordering wins over the tie-break.
    RUVIA_CHECK(select("identity;q=0, gzip;q=0.9, br;q=0.1") == HttpContentCoding::kGzip);
    // A coding at q=0 is excluded even under a permissive wildcard.
    RUVIA_CHECK(select("identity;q=0, br;q=0, *;q=0.5") == HttpContentCoding::kZstd);
    // identity is implicitly q=1, so a lower-quality coding must not override it.
    RUVIA_CHECK(select("gzip;q=0.9") == HttpContentCoding::kIdentity);
    // A positive wildcard does not lower identity's implicit quality, while an
    // explicit identity preference does.
    RUVIA_CHECK(select("*;q=0.5") == HttpContentCoding::kIdentity);
    RUVIA_CHECK(select("identity;q=0.1, gzip;q=0.5") == HttpContentCoding::kGzip);
    // Repeating the same coding is equivalent to multiple matching alternatives:
    // the highest qvalue wins, independently of list order.
    RUVIA_CHECK(select("identity;q=0.5, gzip;q=0.9, gzip;q=0.1") == HttpContentCoding::kGzip);
    RUVIA_CHECK(select("identity;q=0.5, gzip;q=0.1, gzip;q=0.9") == HttpContentCoding::kGzip);
    // The same rule applies to repeated wildcard entries for an unlisted coding.
    RUVIA_CHECK(select("identity;q=0, *;q=0.8, *;q=0.1") == HttpContentCoding::kBrotli);
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
    RUVIA_CHECK(select("identity;q=0.5, gzip;level=9") == HttpContentCoding::kIdentity);
    RUVIA_CHECK(select("identity;q=0.5, gzip;q =1") == HttpContentCoding::kIdentity);
    RUVIA_CHECK(select("identity;q=0.5, gzip;q= 1") == HttpContentCoding::kIdentity);
    // OWS around the weight delimiter itself is explicitly allowed.
    RUVIA_CHECK(select("identity;q=0.5, gzip \t; \tq=0.8") == HttpContentCoding::kGzip);
    // No acceptable coding is an explicit protocol outcome, not identity.
    for (const auto header : {std::string_view{"identity;q=0"}, std::string_view{"*;q=0"}}) {
        HttpResponseCodingQualities qualities;
        qualities.update(header);
        const auto result = HttpResponseCodingSelection::select(qualities);
        RUVIA_CHECK(result.selected() == nullptr);
        RUVIA_CHECK(result.failure() != nullptr);
        if (const auto* failure = result.failure()) {
            RUVIA_CHECK(failure->error() == ruvia::detail::HttpResponseCodingSelectionError::kNoAcceptableCoding);
        }
    }
    HttpResponseCodingQualities explicitEmpty;
    explicitEmpty.update("");
    const auto emptySelectsIdentity = HttpResponseCodingSelection::select(explicitEmpty);
    RUVIA_CHECK(emptySelectsIdentity.selected() != nullptr);
    if (const auto* selected = emptySelectsIdentity.selected()) {
        RUVIA_CHECK(selected->coding() == HttpContentCoding::kIdentity);
        RUVIA_CHECK(selected->identityAccepted());
        RUVIA_CHECK(selected->accepts(HttpContentCoding::kIdentity));
        RUVIA_CHECK(!selected->accepts(HttpContentCoding::kGzip));
    }
    HttpResponseCodingQualities noHeader;
    const auto implicitIdentity = HttpResponseCodingSelection::select(noHeader);
    RUVIA_CHECK(implicitIdentity.selected() != nullptr);
    if (const auto* selected = implicitIdentity.selected()) {
        RUVIA_CHECK(selected->coding() == HttpContentCoding::kIdentity);
        RUVIA_CHECK(selected->identityAccepted());
        RUVIA_CHECK(selected->accepts(HttpContentCoding::kIdentity));
        RUVIA_CHECK(selected->accepts(HttpContentCoding::kGzip));
    }

    auto gzipOnly = HttpResponseCodingCandidates::empty();
    gzipOnly.include(HttpContentCoding::kGzip);
    const auto absentHeaderGzip = HttpResponseCodingSelection::select(noHeader, gzipOnly);
    RUVIA_CHECK(absentHeaderGzip.selected() != nullptr);
    if (const auto* selected = absentHeaderGzip.selected()) {
        RUVIA_CHECK(selected->coding() == HttpContentCoding::kGzip);
        RUVIA_CHECK(selected->accepts(HttpContentCoding::kGzip));
    }

    HttpResponseCodingQualities emptyHeader;
    emptyHeader.update("");
    // An explicitly empty Accept-Encoding allows only an uncoded representation,
    // so a response policy with only gzip has no acceptable candidate.
    const auto explicitEmptyGzip = HttpResponseCodingSelection::select(emptyHeader, gzipOnly);
    RUVIA_CHECK(explicitEmptyGzip.selected() == nullptr);
    RUVIA_CHECK(explicitEmptyGzip.failure() != nullptr);
}

RUVIA_TEST(response_coding_selection_retains_client_preference_until_representation_policy) {
    HttpResponseCodingQualities qualities;
    qualities.update("gzip, identity;q=0");

    // The selector records the client's acceptable representation set. Whether
    // a Web runtime can create gzip is enforced only after the representation
    // source (sidecar, buffered body, or stream) is known.
    const auto selected = HttpResponseCodingSelection::select(qualities);
    RUVIA_CHECK(selected.selected() != nullptr);
    if (const auto* coding = selected.selected()) {
        RUVIA_CHECK(coding->coding() == HttpContentCoding::kGzip);
        RUVIA_CHECK(!coding->identityAccepted());
        RUVIA_CHECK(coding->accepts(HttpContentCoding::kGzip));
        RUVIA_CHECK(!coding->accepts(HttpContentCoding::kIdentity));
    }
}

RUVIA_TEST(response_coding_selection_uses_only_available_representations) {
    HttpResponseCodingQualities qualities;
    qualities.update("br, gzip, identity;q=0");

    auto gzipOnly = HttpResponseCodingCandidates::identityOnly();
    gzipOnly.include(HttpContentCoding::kGzip);
    const auto gzipSelection = HttpResponseCodingSelection::select(qualities, gzipOnly);
    RUVIA_CHECK(gzipSelection.selected() != nullptr);
    if (const auto* selected = gzipSelection.selected()) {
        RUVIA_CHECK(selected->coding() == HttpContentCoding::kGzip);
    }

    auto identityOnly = HttpResponseCodingCandidates::identityOnly();
    const auto identitySelection = HttpResponseCodingSelection::select(qualities, identityOnly);
    RUVIA_CHECK(identitySelection.selected() == nullptr);
    RUVIA_CHECK(identitySelection.failure() != nullptr);

    auto brotliOnly = HttpResponseCodingCandidates::empty();
    brotliOnly.include(HttpContentCoding::kBrotli);
    const auto brotliSelection = HttpResponseCodingSelection::select(qualities, brotliOnly);
    RUVIA_CHECK(brotliSelection.selected() != nullptr);
    if (const auto* selected = brotliSelection.selected()) {
        RUVIA_CHECK(selected->coding() == HttpContentCoding::kBrotli);
    }
}

RUVIA_TEST(response_coding_selection_carries_identity_fallback_once) {
    HttpResponseCodingQualities qualities;
    qualities.update("gzip, identity;q=0");
    const auto selected = HttpResponseCodingSelection::select(qualities);
    RUVIA_CHECK(selected.selected() != nullptr);
    if (const auto* coding = selected.selected()) {
        RUVIA_CHECK(coding->coding() == HttpContentCoding::kGzip);
        RUVIA_CHECK(!coding->identityAccepted());
    }

}
