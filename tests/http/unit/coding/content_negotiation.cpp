#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_content_coding.h"

#include "field/http_quality_value.h"
#include "test_harness.h"

RUVIA_TEST(response_coding_sets_and_selection_snapshots_distinguish_every_supported_coding) {
    constexpr ruvia::http_content_coding codings[]{ruvia::http_content_coding::identity,
        ruvia::http_content_coding::gzip, ruvia::http_content_coding::deflate,
        ruvia::http_content_coding::brotli, ruvia::http_content_coding::zstd};
    for (const auto coding : codings) {
        auto candidates = ruvia::http_response_coding_candidates::empty();
        candidates.include(coding);
        ruvia::http_response_coding_qualities qualities;
        const std::string field = std::string(ruvia::http_content_coding_token(coding)) +
                                  ";q=1,*;q=0" + (coding == ruvia::http_content_coding::identity ? "" : ",identity;q=0");
        qualities.update(field);
        const auto result_value = ruvia::http_response_coding_selection::select(qualities, candidates);
        RUVIA_CHECK(result_value.selected() != nullptr);
        if (const auto* selected = result_value.selected()) {
            RUVIA_CHECK(selected->coding() == coding);
            for (const auto other : codings) {
                RUVIA_CHECK(candidates.contains(other) == (other == coding));
                RUVIA_CHECK(selected->accepts(other) == (other == coding));
                RUVIA_CHECK(ruvia::http_response_coding_candidates::all().contains(other));
            }
        }
    }
}

namespace {

using ruvia::http_accepted_encoding_quality;
using ruvia::http_content_coding;
using ruvia::http_response_coding_candidates;
using ruvia::http_response_coding_qualities;
using ruvia::http_response_coding_selection;
using ruvia::detail::http_parse_quality_value;

// Reference form: one full Accept-Encoding scan per coding. The aggregate
// single-pass update must produce identical qualities.
struct reference_qualities final {
    http_accepted_encoding_quality gzip_;
    http_accepted_encoding_quality brotli_;
    http_accepted_encoding_quality zstd_;
    http_accepted_encoding_quality identity_;
};

reference_qualities reference_three_pass(std::string_view header_value) {
    reference_qualities ref;
    ref.gzip_.update(header_value, "gzip");
    ref.brotli_.update(header_value, "br");
    ref.zstd_.update(header_value, "zstd");
    ref.identity_.update(header_value, "identity");
    return ref;
}

bool same_quality(const http_accepted_encoding_quality& a, const http_accepted_encoding_quality& b) {
    return a.explicit_quality_ == b.explicit_quality_ && a.wildcard_quality_ == b.wildcard_quality_;
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
        "x-gzip, br",
        "X-GZIP;q=0.8, gzip;q=0.2, *;q=0.1",
        "gzip;q=0.2, x-gzip;q=0.8",
        "",
        "deflate;q=0.2",  // unknown coding: leaves all three untouched
        "gzip;q=0, gzip;q=0.9",
        ", gzip, , br,",  // empty items are skipped
        "gzip;q=1.0, br;q=0.500",
        R"(gzip;note="a,b";q=0, br;q=0.5)",
    };
    for (const auto header : cases) {
        http_response_coding_qualities qualities;
        qualities.update(header);

        const auto ref = reference_three_pass(header);
        RUVIA_CHECK(same_quality(qualities.gzip_, ref.gzip_));
        RUVIA_CHECK(same_quality(qualities.brotli_, ref.brotli_));
        RUVIA_CHECK(same_quality(qualities.zstd_, ref.zstd_));
        RUVIA_CHECK(same_quality(qualities.identity_, ref.identity_));
    }
}

RUVIA_TEST(qvalue_parser_rejects_more_than_three_fraction_digits_for_one) {
    RUVIA_CHECK_EQ(http_parse_quality_value("1"), 1000);
    RUVIA_CHECK_EQ(http_parse_quality_value("1."), 1000);
    RUVIA_CHECK_EQ(http_parse_quality_value("1.000"), 1000);
    RUVIA_CHECK_EQ(http_parse_quality_value("0.123"), 123);
    RUVIA_CHECK_EQ(http_parse_quality_value("1.0000"), -1);
    RUVIA_CHECK_EQ(http_parse_quality_value("1.00000"), -1);

    http_response_coding_qualities qualities;
    qualities.update("identity;q=0.5, gzip;q=1.0000");
    const auto result_value = http_response_coding_selection::select(qualities);
    RUVIA_CHECK(result_value.selected() != nullptr);
    if (const auto* selected = result_value.selected()) {
        RUVIA_CHECK(selected->coding() == http_content_coding::identity);
    }
}

RUVIA_TEST(response_coding_selection_end_to_end) {
    const auto select = [](std::string_view header_value) {
        http_response_coding_qualities qualities;
        qualities.update(header_value);
        const auto result_value = http_response_coding_selection::select(qualities);
        const auto* selected = result_value.selected();
        if (selected == nullptr) {
            throw std::runtime_error("test expected an acceptable response coding");
        }
        return selected->coding();
    };
    // Server tie-break prefers br > zstd > gzip > deflate at equal q.
    RUVIA_CHECK(select("gzip, br, zstd") == http_content_coding::brotli);
    // Explicit q ordering wins over the tie-break.
    RUVIA_CHECK(select("identity;q=0, gzip;q=0.9, br;q=0.1") == http_content_coding::gzip);
    // A coding at q=0 is excluded even under a permissive wildcard.
    RUVIA_CHECK(select("identity;q=0, br;q=0, *;q=0.5") == http_content_coding::zstd);
    // identity is implicitly q=1, so a lower-quality coding must not override it.
    RUVIA_CHECK(select("gzip;q=0.9") == http_content_coding::identity);
    // A positive wildcard does not lower identity's implicit quality, while an
    // explicit identity preference does.
    RUVIA_CHECK(select("*;q=0.5") == http_content_coding::identity);
    RUVIA_CHECK(select("identity;q=0.1, gzip;q=0.5") == http_content_coding::gzip);
    RUVIA_CHECK(select("identity;q=0.1, deflate;q=0.5") == http_content_coding::deflate);
    // Repeating the same coding is equivalent to multiple matching alternatives:
    // the highest qvalue wins, independently of list order.
    RUVIA_CHECK(select("identity;q=0.5, gzip;q=0.9, gzip;q=0.1") == http_content_coding::gzip);
    RUVIA_CHECK(select("identity;q=0.5, gzip;q=0.1, gzip;q=0.9") == http_content_coding::gzip);
    // The same rule applies to repeated wildcard entries for an unlisted coding.
    RUVIA_CHECK(select("identity;q=0, *;q=0.8, *;q=0.1") == http_content_coding::brotli);
    http_response_coding_qualities repeated;
    repeated.update("gzip;q=0.9, gzip;q=0.1, *;q=0.8, *;q=0.2");
    RUVIA_CHECK_EQ(repeated.gzip_.explicit_quality_, 900);
    RUVIA_CHECK_EQ(repeated.brotli_.wildcard_quality_, 800);
    http_response_coding_qualities split_lines;
    split_lines.update("gzip;q=0.9, *;q=0.8");
    split_lines.update("gzip;q=0.1, *;q=0.2");
    RUVIA_CHECK_EQ(split_lines.gzip_.explicit_quality_, 900);
    RUVIA_CHECK_EQ(split_lines.brotli_.wildcard_quality_, 800);
    // Accept-Encoding allows only an optional weight after a coding. Unknown
    // parameters and whitespace around q's '=' make the item invalid; they must
    // not inherit the default q=1 and outrank identity.
    RUVIA_CHECK(select("identity;q=0.5, gzip;level=9") == http_content_coding::identity);
    RUVIA_CHECK(select("identity;q=0.5, gzip;q =1") == http_content_coding::identity);
    RUVIA_CHECK(select("identity;q=0.5, gzip;q= 1") == http_content_coding::identity);
    // OWS around the weight delimiter itself is explicitly allowed.
    RUVIA_CHECK(select("identity;q=0.5, gzip \t; \tq=0.8") == http_content_coding::gzip);
    // No acceptable coding is an explicit protocol outcome, not identity.
    for (const auto header : {std::string_view{"identity;q=0"}, std::string_view{"*;q=0"}}) {
        http_response_coding_qualities qualities;
        qualities.update(header);
        const auto result_value = http_response_coding_selection::select(qualities);
        RUVIA_CHECK(result_value.selected() == nullptr);
        RUVIA_CHECK(result_value.failure() != nullptr);
        if (const auto* failure = result_value.failure()) {
            RUVIA_CHECK(failure->error() ==
                        ruvia::http_response_coding_selection_error::no_acceptable_coding);
        }
    }
    http_response_coding_qualities explicit_empty;
    explicit_empty.update("");
    const auto empty_selects_identity = http_response_coding_selection::select(explicit_empty);
    RUVIA_CHECK(empty_selects_identity.selected() != nullptr);
    if (const auto* selected = empty_selects_identity.selected()) {
        RUVIA_CHECK(selected->coding() == http_content_coding::identity);
        RUVIA_CHECK(selected->identity_accepted());
        RUVIA_CHECK(selected->accepts(http_content_coding::identity));
        RUVIA_CHECK(!selected->accepts(http_content_coding::gzip));
    }
    http_response_coding_qualities no_header;
    const auto implicit_identity = http_response_coding_selection::select(no_header);
    RUVIA_CHECK(implicit_identity.selected() != nullptr);
    if (const auto* selected = implicit_identity.selected()) {
        RUVIA_CHECK(selected->coding() == http_content_coding::identity);
        RUVIA_CHECK(selected->identity_accepted());
        RUVIA_CHECK(selected->accepts(http_content_coding::identity));
        RUVIA_CHECK(selected->accepts(http_content_coding::gzip));
    }

    auto gzip_only = http_response_coding_candidates::empty();
    gzip_only.include(http_content_coding::gzip);
    const auto absent_header_gzip = http_response_coding_selection::select(no_header, gzip_only);
    RUVIA_CHECK(absent_header_gzip.selected() != nullptr);
    if (const auto* selected = absent_header_gzip.selected()) {
        RUVIA_CHECK(selected->coding() == http_content_coding::gzip);
        RUVIA_CHECK(selected->accepts(http_content_coding::gzip));
    }

    http_response_coding_qualities empty_header;
    empty_header.update("");
    // An explicitly empty Accept-Encoding allows only an uncoded representation,
    // so a response policy with only gzip has no acceptable candidate.
    const auto explicit_empty_gzip = http_response_coding_selection::select(empty_header, gzip_only);
    RUVIA_CHECK(explicit_empty_gzip.selected() == nullptr);
    RUVIA_CHECK(explicit_empty_gzip.failure() != nullptr);
}

RUVIA_TEST(accepted_encoding_quality_treats_gzip_aliases_as_the_same_coding) {
    for (const auto coding : {std::string_view{"gzip"}, std::string_view{"x-gzip"}, std::string_view{"X-GZIP"}}) {
        RUVIA_CHECK(ruvia::http_accepts_encoding("x-gzip", coding));
        RUVIA_CHECK(ruvia::http_accepts_encoding("GZIP", coding));
        RUVIA_CHECK(!ruvia::http_accepts_encoding("x-gzip;q=0, *;q=1", coding));
        RUVIA_CHECK(!ruvia::http_accepts_encoding("gzip;q=0, *;q=1", coding));
        RUVIA_CHECK(ruvia::http_accepts_encoding("x-gzip;q=0.8, *;q=0", coding));
    }
}

RUVIA_TEST(response_coding_selection_accepts_the_gzip_alias) {
    for (const auto header : {std::string_view{"x-gzip, identity;q=0"}, std::string_view{"X-GZIP;q=0.8, identity;q=0"}}) {
        http_response_coding_qualities qualities;
        qualities.update(header);
        const auto result_value = http_response_coding_selection::select(qualities);
        RUVIA_CHECK(result_value.failure() == nullptr);
        RUVIA_CHECK(result_value.selected() != nullptr);
        if (const auto* selected = result_value.selected()) {
            RUVIA_CHECK(selected->coding() == http_content_coding::gzip);
            RUVIA_CHECK(selected->accepts(http_content_coding::gzip));
            RUVIA_CHECK(!selected->identity_accepted());
            RUVIA_CHECK(!selected->accepts(http_content_coding::identity));
            RUVIA_CHECK_EQ(ruvia::http_content_coding_token(selected->coding()), "gzip");
        }
    }
}

RUVIA_TEST(response_coding_gzip_aliases_share_weights_across_field_lines) {
    for (const bool alias_first : {false, true}) {
        const auto first = alias_first ? "x-gzip;q=0.8" : "gzip;q=0.2";
        const auto second = alias_first ? "gzip;q=0.2" : "X-GZIP;q=0.8";
        http_response_coding_qualities qualities;
        qualities.update(first);
        qualities.update(second);
        RUVIA_CHECK_EQ(qualities.gzip_.explicit_quality_, 800);
        for (const auto coding : {std::string_view{"gzip"}, std::string_view{"x-gzip"}}) {
            http_accepted_encoding_quality quality;
            quality.update(first, coding);
            quality.update(second, coding);
            RUVIA_CHECK_EQ(quality.explicit_quality_, qualities.gzip_.explicit_quality_);
        }
    }
}

RUVIA_TEST(response_coding_selection_retains_client_preference_until_representation_policy) {
    http_response_coding_qualities qualities;
    qualities.update("gzip, identity;q=0");

    // The selector records the client's acceptable representation set. Whether
    // a Web runtime can create gzip is enforced only after the representation
    // source (sidecar, buffered body, or stream) is known.
    const auto selected = http_response_coding_selection::select(qualities);
    RUVIA_CHECK(selected.selected() != nullptr);
    if (const auto* coding = selected.selected()) {
        RUVIA_CHECK(coding->coding() == http_content_coding::gzip);
        RUVIA_CHECK(!coding->identity_accepted());
        RUVIA_CHECK(coding->accepts(http_content_coding::gzip));
        RUVIA_CHECK(!coding->accepts(http_content_coding::identity));
    }
}

RUVIA_TEST(response_coding_selection_uses_only_available_representations) {
    http_response_coding_qualities qualities;
    qualities.update("br, gzip, identity;q=0");

    auto gzip_only = http_response_coding_candidates::identity_only();
    gzip_only.include(http_content_coding::gzip);
    const auto gzip_selection = http_response_coding_selection::select(qualities, gzip_only);
    RUVIA_CHECK(gzip_selection.selected() != nullptr);
    if (const auto* selected = gzip_selection.selected()) {
        RUVIA_CHECK(selected->coding() == http_content_coding::gzip);
    }

    auto identity_only = http_response_coding_candidates::identity_only();
    const auto identity_selection = http_response_coding_selection::select(qualities, identity_only);
    RUVIA_CHECK(identity_selection.selected() == nullptr);
    RUVIA_CHECK(identity_selection.failure() != nullptr);

    auto brotli_only = http_response_coding_candidates::empty();
    brotli_only.include(http_content_coding::brotli);
    const auto brotli_selection = http_response_coding_selection::select(qualities, brotli_only);
    RUVIA_CHECK(brotli_selection.selected() != nullptr);
    if (const auto* selected = brotli_selection.selected()) {
        RUVIA_CHECK(selected->coding() == http_content_coding::brotli);
    }
}

RUVIA_TEST(response_coding_selection_carries_identity_fallback_once) {
    http_response_coding_qualities qualities;
    qualities.update("gzip, identity;q=0");
    const auto selected = http_response_coding_selection::select(qualities);
    RUVIA_CHECK(selected.selected() != nullptr);
    if (const auto* coding = selected.selected()) {
        RUVIA_CHECK(coding->coding() == http_content_coding::gzip);
        RUVIA_CHECK(!coding->identity_accepted());
    }
}

RUVIA_TEST(http_accepts_encoding_rfc9110_identity_rules) {
    using ruvia::http_accepts_encoding;

    // RFC 9110 §12.5.3: Empty field-value accepts only identity.
    RUVIA_CHECK(http_accepts_encoding("", "identity"));
    RUVIA_CHECK(!http_accepts_encoding("", "gzip"));

    // RFC 9110 §12.5.3: "identity" is always acceptable unless specifically refused.
    RUVIA_CHECK(http_accepts_encoding("gzip", "identity"));
    RUVIA_CHECK(http_accepts_encoding("gzip, br", "identity"));
    RUVIA_CHECK(http_accepts_encoding("*;q=0.5", "identity"));

    // Explicitly refused with identity;q=0.
    RUVIA_CHECK(!http_accepts_encoding("identity;q=0", "identity"));
    RUVIA_CHECK(!http_accepts_encoding("gzip, identity;q=0", "identity"));

    // Refused by wildcard *;q=0 when identity is not specifically included.
    RUVIA_CHECK(!http_accepts_encoding("*;q=0", "identity"));
    RUVIA_CHECK(!http_accepts_encoding("gzip, *;q=0", "identity"));

    // Explicit identity;q>0 overrides *;q=0.
    RUVIA_CHECK(http_accepts_encoding("gzip, *;q=0, identity;q=0.5", "identity"));

    // Explicit identity;q>0.
    RUVIA_CHECK(http_accepts_encoding("identity;q=0.1", "identity"));

    // http_accepted_encoding_quality accepts(bool) overload check.
    http_accepted_encoding_quality quality;
    quality.update("gzip", "identity");
    RUVIA_CHECK(quality.accepts(true));
    RUVIA_CHECK(!quality.accepts(false));
}
