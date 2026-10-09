#include <initializer_list>
#include <string_view>

#include "ruvia/http/http_accept_match.h"

#include "field/http_accept_media_type.h"
#include "field/http_quality_value.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http_accepts_media_type;
using ruvia::detail::http_media_range_matches;
using ruvia::detail::http_parse_quality_value;
using ruvia::detail::http_quality_parameter;

}  // namespace

RUVIA_TEST(http_accept_match_accumulates_fields_and_respects_specific_exclusion) {
    ruvia::http_accept_match media;
    media.update_media_type("text/*;q=0.8", "text/html");
    RUVIA_CHECK(media.matched());
    RUVIA_CHECK_EQ(media.quality(), 800);
    media.update_media_type("text/html;q=0", "text/html");
    RUVIA_CHECK(!media.matched());
    RUVIA_CHECK_EQ(media.quality(), 0);

    ruvia::http_accept_match language;
    language.update_token("en;q=0.5", "en-US", ruvia::http_accept_token_match_mode::language_prefix);
    RUVIA_CHECK(language.matched());
    RUVIA_CHECK_EQ(language.quality(), 500);
}

RUVIA_TEST(http_accept_match_language_uses_longest_matching_range) {
    constexpr auto mode = ruvia::http_accept_token_match_mode::language_prefix;
    struct language_case final {
        std::string_view field_;
        int quality_;
    };
    constexpr language_case cases[] = {
        {"zh;q=1, zh-Hant;q=0", 0},
        {"zh-Hant;q=0, zh;q=1", 0},
        {"zh;q=1, zh-Hant;q=0.6", 600},
        {"zh-Hant;q=0.6, zh;q=1", 600},
        {"zh;q=0, ZH-hant;q=0.6", 600},
        {"*;q=1, zh-Hant;q=0", 0},
        {"zh-Hant;q=0.3, zh-Hant;q=0.8", 800},
        {"zh-Hant;q=0.8, zh-Hant;q=0.3", 800},
        {"zh-Hant;q=1, zh-Hant-TW;q=0", 0},
        {"zh-Hant;q=0, zh-Hant-TW;q=0.4", 400},
    };
    for (const auto& item : cases) {
        ruvia::http_accept_match match;
        match.update_token(item.field_, "zh-Hant-TW", mode);
        RUVIA_CHECK_EQ(match.quality(), item.quality_);
        RUVIA_CHECK_EQ(match.matched(), item.quality_ > 0);
    }
}

RUVIA_TEST(http_accept_match_language_combines_field_lines) {
    constexpr auto mode = ruvia::http_accept_token_match_mode::language_prefix;
    for (const bool specific_first : {false, true}) {
        ruvia::http_accept_match split;
        split.update_token(specific_first ? "de-DE;q=0" : "de;q=1", "de-DE-1996", mode);
        split.update_token(specific_first ? "de;q=1" : "de-DE;q=0", "de-DE-1996", mode);
        ruvia::http_accept_match joined;
        joined.update_token("de;q=1, de-DE;q=0", "de-DE-1996", mode);
        RUVIA_CHECK_EQ(split.quality(), joined.quality());
        RUVIA_CHECK(!split.matched());
    }

    ruvia::http_accept_match exact;
    exact.update_token("de-DE;q=1", "de-DE-1996", ruvia::http_accept_token_match_mode::exact);
    RUVIA_CHECK(!exact.matched());
    exact.update_token("*;q=0.8", "de-DE-1996", ruvia::http_accept_token_match_mode::exact);
    RUVIA_CHECK_EQ(exact.quality(), 800);
    exact.update_token("DE-de-1996;q=0.2", "de-DE-1996", ruvia::http_accept_token_match_mode::exact);
    RUVIA_CHECK_EQ(exact.quality(), 200);
}

RUVIA_TEST(http_accept_match_token_fields_allow_only_an_optional_weight) {
    struct token_weight_case final {
        std::string_view field_;
        int quality_;
    };
    constexpr token_weight_case cases[] = {
        {"en", 1000},
        {"EN \t; \tQ=0.5", 500},
        {"en;q=0", 0},
        {"en;q=0.999", 999},
        {"en;level=1", 0},
        {"en;q=0.5;level=1", 0},
        {"en;level=1;q=0.5", 0},
        {"en;q=\"0.5\"", 0},
        {"en;q=0.8;q=0.6", 0},
        {"en;q =1", 0},
        {"en;q= 1", 0},
        {"en;q=0.1234", 0},
        {"*;level=1", 0},
        {"*;q=0.5;level=1", 0},
        {"*;q=0.4", 400},
    };
    for (const auto mode : {ruvia::http_accept_token_match_mode::exact, ruvia::http_accept_token_match_mode::language_prefix}) {
        const auto offered = mode == ruvia::http_accept_token_match_mode::exact ? "en" : "en-US";
        for (const auto& item : cases) {
            ruvia::http_accept_match match;
            match.update_token(item.field_, offered, mode);
            RUVIA_CHECK_EQ(match.quality(), item.quality_);
            RUVIA_CHECK_EQ(match.matched(), item.quality_ > 0);
        }
    }
}

RUVIA_TEST(http_accept_match_token_weights_accumulate_across_field_lines) {
    for (const auto mode : {ruvia::http_accept_token_match_mode::exact, ruvia::http_accept_token_match_mode::language_prefix}) {
        const auto offered = mode == ruvia::http_accept_token_match_mode::exact ? "en" : "en-US";
        for (const bool invalid_first : {false, true}) {
            ruvia::http_accept_match match;
            match.update_token(invalid_first ? "en;level=1" : "en;q=0.6", offered, mode);
            match.update_token(invalid_first ? "en;q=0.6" : "en;level=1", offered, mode);
            RUVIA_CHECK(match.matched());
            RUVIA_CHECK_EQ(match.quality(), 600);
        }
    }
}

RUVIA_TEST(parse_quality_value_rfc7231_grammar) {
    // qvalue = ( "0" [ "." 0*3DIGIT ] ) / ( "1" [ "." 0*3("0") ] ), mapped to
    // milli-units 0..1000.
    RUVIA_CHECK_EQ(http_parse_quality_value("1"), 1000);
    RUVIA_CHECK_EQ(http_parse_quality_value("0"), 0);
    RUVIA_CHECK_EQ(http_parse_quality_value("0.5"), 500);
    RUVIA_CHECK_EQ(http_parse_quality_value("0.500"), 500);
    RUVIA_CHECK_EQ(http_parse_quality_value("0.123"), 123);
    RUVIA_CHECK_EQ(http_parse_quality_value("0.999"), 999);
    RUVIA_CHECK_EQ(http_parse_quality_value("1.0"), 1000);
    RUVIA_CHECK_EQ(http_parse_quality_value("1.000"), 1000);

    // Invalid qvalues yield -1: greater than 1, a non-zero fraction on 1.x, more
    // than three fraction digits, an out-of-range integer, and non-numeric input.
    RUVIA_CHECK_EQ(http_parse_quality_value("1.0000"), -1);
    RUVIA_CHECK_EQ(http_parse_quality_value("1.5"), -1);
    RUVIA_CHECK_EQ(http_parse_quality_value("1.1"), -1);
    RUVIA_CHECK_EQ(http_parse_quality_value("0.1234"), -1);
    RUVIA_CHECK_EQ(http_parse_quality_value("2"), -1);
    RUVIA_CHECK_EQ(http_parse_quality_value("abc"), -1);
    RUVIA_CHECK_EQ(http_parse_quality_value(""), -1);
}

RUVIA_TEST(quality_parameter_extracts_q_from_header_item) {
    // A header item without a q parameter defaults to full quality (1000): the
    // media range / coding is present and acceptable.
    RUVIA_CHECK_EQ(http_quality_parameter("gzip"), 1000);
    RUVIA_CHECK_EQ(http_quality_parameter("text/html"), 1000);

    // The q parameter is extracted and its name matched case-insensitively.
    RUVIA_CHECK_EQ(http_quality_parameter("gzip;q=0.5"), 500);
    RUVIA_CHECK_EQ(http_quality_parameter("text/plain;Q=0.8"), 800);

    // A syntactically INVALID q collapses to 0 (not acceptable) rather than the
    // present-default of 1000 -- a malformed q must never be read as "accept".
    RUVIA_CHECK_EQ(http_quality_parameter("gzip;q=2"), 0);
    RUVIA_CHECK_EQ(http_quality_parameter("gzip;q=bogus"), 0);

    // A media parameter name cannot occur more than once; duplicate q is invalid.
    RUVIA_CHECK_EQ(http_quality_parameter("gzip;q=0.3;q=0.9"), 0);

    // A ';' inside a quoted parameter value is not a parameter separator, so a
    // fake q smuggled inside quotes is ignored and the real trailing q is used.
    RUVIA_CHECK_EQ(http_quality_parameter(R"(a;note="x;q=1";q=0.4)"), 400);

    // Parameter grammar never permits whitespace around '='.
    RUVIA_CHECK_EQ(http_quality_parameter("text/html;q =1"), 0);
    RUVIA_CHECK_EQ(http_quality_parameter("text/html;q= 1"), 0);
}

RUVIA_TEST(media_range_matches_type_subtype_and_wildcards) {
    RUVIA_CHECK(http_media_range_matches("text/html", "text/html"));
    RUVIA_CHECK(http_media_range_matches("text/*", "text/html"));
    RUVIA_CHECK(http_media_range_matches("*/*", "application/json"));
    RUVIA_CHECK(http_media_range_matches("TEXT/HTML", "text/html"));  // case-insensitive
    RUVIA_CHECK(http_media_range_matches("text/html;charset=utf-8", "text/html;charset=\"utf-8\""));
    // Charset names are registered case-insensitively; quoted-string syntax
    // does not change that comparison rule.
    RUVIA_CHECK(http_media_range_matches("text/html;charset=\"UTF-8\"", "text/html;CHARSET=utf-8"));
    RUVIA_CHECK(!http_media_range_matches("text/html;charset=utf-8", "text/html"));
    RUVIA_CHECK(!http_media_range_matches("text/html;charset=utf-8", "text/html;charset=iso-8859-1"));
    // Other parameter values retain their registered case-sensitive semantics.
    RUVIA_CHECK(!http_media_range_matches(
        "application/json;profile=Example", "application/json;profile=example"));
    RUVIA_CHECK(!http_media_range_matches("text/*", "application/json"));  // type mismatch
    RUVIA_CHECK(!http_media_range_matches("text/plain", "text/html"));     // subtype mismatch
    RUVIA_CHECK(!http_media_range_matches("text", "text/html"));           // no slash -> invalid
}

RUVIA_TEST(media_range_specificity_ordering) {
    // Full type/subtype overrides type/*, which overrides */*, even at lower q.
    constexpr std::string_view field = "*/*;q=0.9, text/*;q=0.7, text/html;q=0.2";
    ruvia::http_accept_match exact;
    exact.update_media_type(field, "text/html");
    RUVIA_CHECK_EQ(exact.quality(), 200);
    ruvia::http_accept_match subtype_wildcard;
    subtype_wildcard.update_media_type(field, "text/plain");
    RUVIA_CHECK_EQ(subtype_wildcard.quality(), 700);
    ruvia::http_accept_match wildcard;
    wildcard.update_media_type(field, "application/json");
    RUVIA_CHECK_EQ(wildcard.quality(), 900);
    RUVIA_CHECK(!http_media_range_matches("bogus", "text/html"));
}

RUVIA_TEST(media_range_rejects_invalid_tokens) {
    RUVIA_CHECK(!http_media_range_matches("text/", "text/html"));
    RUVIA_CHECK(!http_media_range_matches("/json", "application/json"));
    RUVIA_CHECK(!http_media_range_matches("*/json", "application/json"));
    RUVIA_CHECK(!http_media_range_matches("text /html", "text/html"));

    RUVIA_CHECK(!http_media_range_matches("text/*", "text/"));
    RUVIA_CHECK(!http_accepts_media_type("*/json, */*;q=0", "application/json"));
}

RUVIA_TEST(media_range_rejects_whitespace_around_parameter_equals) {
    RUVIA_CHECK(!http_accepts_media_type("text/html;q =1", "text/html"));
    RUVIA_CHECK(!http_accepts_media_type("text/html;level =1", "text/html;level=1"));
    RUVIA_CHECK(!http_accepts_media_type("text/html;charset=utf-8", "text/html;charset =utf-8"));

    // OWS around the semicolon delimiter remains legal.
    RUVIA_CHECK(http_accepts_media_type("text/html \t; \tq=0.5", "text/html"));
}

RUVIA_TEST(media_range_rejects_duplicate_parameter_names) {
    RUVIA_CHECK(!http_accepts_media_type("text/html;level=1;LEVEL=1", "text/html;level=1"));
    RUVIA_CHECK(!http_accepts_media_type("text/html;q=1;Q=0", "text/html"));
    RUVIA_CHECK(!http_accepts_media_type("text/html;level=1", "text/html;level=1;LEVEL=2"));
}

RUVIA_TEST(media_range_rejects_invalid_offered_parameters) {
    for (const std::string_view offered :
        {"text/plain; charset", "text/plain; charset=", "text/plain; charset =utf-8",
            "text/plain; charset=utf-8; CHARSET=latin1", "text/plain; charset=\"unterminated"}) {
        RUVIA_CHECK(!http_media_range_matches("*/*", offered));
        RUVIA_CHECK(!http_accepts_media_type("*/*", offered));
        RUVIA_CHECK(!http_accepts_media_type("", offered));
    }
}

RUVIA_TEST(http_accept_match_media_parameters_decide_specificity_across_fields) {
    constexpr std::string_view offered = "text/plain;charset=utf-8;format=flowed";
    constexpr std::string_view less_specific = "text/plain;format=flowed;q=0.8";
    constexpr std::string_view more_specific = "text/plain;CHARSET=\"UTF-8\";format=flowed;q=0.2";
    for (const bool specific_first : {false, true}) {
        ruvia::http_accept_match match;
        match.update_media_type(specific_first ? more_specific : less_specific, offered);
        match.update_media_type(specific_first ? less_specific : more_specific, offered);
        RUVIA_CHECK_EQ(match.quality(), 200);
    }
    ruvia::http_accept_match joined;
    joined.update_media_type("text/plain;format=flowed;q=0.8, text/plain;CHARSET=\"UTF-8\";format=flowed;q=0.2", offered);
    RUVIA_CHECK_EQ(joined.quality(), 200);

    // q is a weight, not an additional matching media parameter.
    for (const bool weighted_first : {false, true}) {
        ruvia::http_accept_match match;
        match.update_media_type(weighted_first ? "text/plain;q=0.9" : "text/plain", offered);
        match.update_media_type(weighted_first ? "text/plain" : "text/plain;q=0.9", offered);
        RUVIA_CHECK_EQ(match.quality(), 1000);
    }
}

RUVIA_TEST(http_accept_match_media_invalid_parameter_suffix_preserves_the_best_match) {
    constexpr std::string_view offered = "text/plain;charset=utf-8;format=flowed";
    for (const std::string_view field : {
             "text/plain;format=flowed;FORMAT=flowed;q=1",
             "text/plain;format=flowed;charset=\"unterminated",
             "text/plain;format=flowed;charset;q=1",
             "text/plain;format=flowed;unknown=present;q=1"}) {
        ruvia::http_accept_match match;
        match.update_media_type("text/plain;q=0.5", offered);
        match.update_media_type(field, offered);
        RUVIA_CHECK_EQ(match.quality(), 500);
    }
}

RUVIA_TEST(http_accept_match_media_parameter_lookup_handles_order_and_quoted_values) {
    constexpr std::string_view field = R"(text/plain;note="a;b=c";format="flowed";profile=aB;charset=utf-8;q=0.8)";
    for (const std::string_view offered : {
             R"(text/plain;FORMAT=flowed;note="a;b=c";profile="\a\B";CHARSET="UTF-8")",
             R"(text/plain;CHARSET="UTF-8";profile="\a\B";note="a;b=c";FORMAT=flowed)",
             R"(text/plain;profile=aB;note="a;b=c";charset=utf-8;format="flowed")"}) {
        ruvia::http_accept_match match;
        match.update_media_type(field, offered);
        RUVIA_CHECK_EQ(match.quality(), 800);
        RUVIA_CHECK(http_media_range_matches(field, offered));
        RUVIA_CHECK(!http_accepts_media_type("text/plain;profile=AB;q=1", offered));
        RUVIA_CHECK(!http_accepts_media_type("text/plain;missing=value;q=1", offered));
    }
}

RUVIA_TEST(accepts_media_type_requires_a_valid_complete_offered_value) {
    constexpr std::string_view field = "text/plain;charset=utf-8;q=1";
    for (const std::string_view offered : {
             "text/plain;charset=utf-8;profile",
             "text/plain;charset=utf-8;profile=",
             "text/plain;charset=utf-8;profile=\"unterminated",
             "text/plain;charset=utf-8;CHARSET=utf-8",
             "text/plain;charset=utf-8;CHARSET=latin1",
             "text/plain;charset=utf-8;profile =example"}) {
        ruvia::http_accept_match match;
        match.update_media_type(field, offered);
        RUVIA_CHECK(!match.matched());
        RUVIA_CHECK_EQ(match.quality(), 0);
        RUVIA_CHECK(!http_media_range_matches(field, offered));
        RUVIA_CHECK(!http_accepts_media_type(field, offered));
    }
}

RUVIA_TEST(accepts_media_type_basic) {
    RUVIA_CHECK(http_accepts_media_type("", "text/html"));  // absent Accept -> accept anything
    RUVIA_CHECK(http_accepts_media_type("text/html", "text/html"));
    RUVIA_CHECK(http_accepts_media_type("text/*", "text/html"));
    RUVIA_CHECK(http_accepts_media_type("*/*", "application/json"));
    RUVIA_CHECK(!http_accepts_media_type("text/*", "application/json"));  // type mismatch
    RUVIA_CHECK(!http_accepts_media_type("text/html;q=0", "text/html"));  // explicit q=0 rejects
}

RUVIA_TEST(accepts_media_type_specificity_beats_quality) {
    // RFC 7231 5.3.2: the MOST SPECIFIC matching range decides, even when a
    // broader range carries a higher q. A specific text/html;q=0 excludes the
    // type despite text/*;q=1.
    RUVIA_CHECK(!http_accepts_media_type("text/*;q=1.0, text/html;q=0", "text/html"));
    // A specific positive q wins over a broader q=0.
    RUVIA_CHECK(http_accepts_media_type("*/*;q=0, text/html;q=0.5", "text/html"));
    // A broader q=0.5 does not rescue a specific q=0.
    RUVIA_CHECK(!http_accepts_media_type("*/*;q=0.5, text/html;q=0", "text/html"));
    // Highest q among equally-specific matches is taken.
    RUVIA_CHECK(http_accepts_media_type("text/plain;q=0.5, text/html;q=0.8", "text/html"));
}

RUVIA_TEST(accepts_media_type_parameters_participate_in_matching_and_precedence) {
    // A parameterized range must not match a representation with a different
    // parameter. The generic q=0 range therefore remains the winning match.
    RUVIA_CHECK(!http_accepts_media_type(
        "application/json;profile=v2;q=1, application/json;q=0", "application/json;profile=v1"));

    // When the parameter does match, that range is more specific than the bare
    // media type and its quality controls acceptance.
    RUVIA_CHECK(http_accepts_media_type(
        "application/json;profile=v1;q=0.7, application/json;q=0", "application/json;profile=v1"));
    RUVIA_CHECK(!http_accepts_media_type(
        "application/json;profile=v1;q=0, application/json;q=1", "application/json;profile=v1"));

    // Media-type parameter names are case-insensitive and quoted token-equivalent
    // values compare after quoted-pair decoding. RFC 9110 removed accept-ext, so
    // parameters after q still constrain the media range.
    RUVIA_CHECK(http_accepts_media_type(R"(text/plain;FORMAT="flowed";q=0.5;extension=ignored)",
        "text/plain;format=flowed;extension=ignored"));
    RUVIA_CHECK(!http_accepts_media_type(R"(text/plain;q=0.5;format="flowed")", "text/plain"));
    RUVIA_CHECK(
        http_accepts_media_type(R"(text/plain;q=0.5;format="flowed")", "text/plain;format=flowed"));
}
