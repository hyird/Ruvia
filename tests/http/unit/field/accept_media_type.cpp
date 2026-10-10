#include <initializer_list>
#include <string_view>

#include "ruvia/http/http_accept_match.h"

#include "test_harness.h"

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
        {"en;", 0},
        {"en;;q=0.5", 0},
        {"en;q=0.5;", 0},
        {"*;;", 0},
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
    }
}

RUVIA_TEST(http_accept_match_media_empty_parameter_slots_preserve_quality) {
    struct media_case final {
        std::string_view field_;
        int quality_;
    };
    constexpr media_case cases[] = {
        {"", 0},
        {", ,", 0},
        {"text/plain;", 1000},
        {"text/plain; \t; charset=utf-8;;", 1000},
        {"text/plain;;q=0.6; ;", 600},
        {"text/plain;;q=0;;", 0},
        {"text/*;;q=0.4;", 400},
        {"*/*;;q=0.3;", 300},
        {"text/plain;;q=;", 0},
        {"text/plain;;q=\"0.6\";", 0},
        {"text/plain;;q=0.6;;Q=0.8;", 0},
        {"text/plain;;q=0.1234;", 0},
        {"text/plain;;charset=;q=1;", 0},
        {"text/plain;;charset =utf-8;q=1;", 0},
        {"text/plain;;charset= utf-8;q=1;", 0},
        {"text/plain;;charset=latin1;q=1;", 0},
    };
    for (const std::string_view offered : {"text/plain;charset=utf-8", "text/plain;;charset=utf-8; ;"}) {
        for (const auto& item : cases) {
            ruvia::http_accept_match match;
            match.update_media_type(item.field_, offered);
            RUVIA_CHECK_EQ(match.quality(), item.quality_);
            RUVIA_CHECK_EQ(match.matched(), item.quality_ > 0);
        }
    }
}

RUVIA_TEST(http_accept_match_media_empty_slots_preserve_specificity_across_fields) {
    constexpr std::string_view general = "text/plain;;q=0.8;";
    constexpr std::string_view specific = "text/plain; ;charset=utf-8;;q=0;";
    for (const bool specific_first : {false, true}) {
        ruvia::http_accept_match excluded;
        excluded.update_media_type(specific_first ? specific : general, "text/plain;;charset=utf-8;");
        excluded.update_media_type(specific_first ? general : specific, "text/plain;;charset=utf-8;");
        RUVIA_CHECK(!excluded.matched());

        ruvia::http_accept_match other_charset;
        other_charset.update_media_type(specific_first ? specific : general, "text/plain;;charset=latin1;");
        other_charset.update_media_type(specific_first ? general : specific, "text/plain;;charset=latin1;");
        RUVIA_CHECK_EQ(other_charset.quality(), 800);
    }
    for (const bool higher_first : {false, true}) {
        ruvia::http_accept_match repeated;
        repeated.update_media_type(higher_first ? "text/plain;;q=0.8;" : "text/plain;q=0.2", "text/plain;");
        repeated.update_media_type(higher_first ? "text/plain;q=0.2" : "text/plain;;q=0.8;", "text/plain;");
        RUVIA_CHECK_EQ(repeated.quality(), 800);
    }
}
