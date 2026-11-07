#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <string_view>

#include "ruvia/http/http_accept_match.h"

namespace {

std::string media_type_with_parameters(int count) {
    std::string value = "text/plain";
    for (int index = 0; index < count; ++index) {
        value += ";p" + std::to_string(index) + "=\"a;b\"";
    }
    return value;
}

}  // namespace

TEST(Ruvia, http_media_type_parameters_up_to_the_cap_are_matched) {
    const auto full = media_type_with_parameters(64);
    EXPECT_TRUE(ruvia::is_valid_http_content_type_field_value(full));

    ruvia::http_accept_match wildcard;
    wildcard.update_media_type("*/*", full);
    EXPECT_EQ(wildcard.quality(), 1000);

    ruvia::http_accept_match tail;
    tail.update_media_type("text/plain;p63=\"a;b\";q=0.4", full);
    EXPECT_EQ(tail.quality(), 400);

    // One slot below the cap leaves room for q in the media range.
    const auto offered = media_type_with_parameters(63);
    ruvia::http_accept_match specific;
    specific.update_media_type("text/plain;q=0.9", offered);
    specific.update_media_type(offered + ";q=0.2", offered);
    EXPECT_EQ(specific.quality(), 200);

    EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value(offered + ";p62=v"));
    EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value(offered + ";P0=v"));
    EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value(offered + ";broken=\"unterminated"));

    ruvia::http_accept_match malformed;
    malformed.update_media_type("text/*;q=0.7", offered);
    malformed.update_media_type(offered + ";broken=", offered);
    EXPECT_EQ(malformed.quality(), 700);
}

TEST(Ruvia, http_media_type_parameters_beyond_the_cap_are_invalid) {
    for (const int count : {65, 512}) {
        const auto offered = media_type_with_parameters(count);
        EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value(offered));

        ruvia::http_accept_match wildcard;
        wildcard.update_media_type("*/*", offered);
        EXPECT_TRUE(!wildcard.matched());
    }

    // q counts toward the cap: the over-long range is ignored, the shorter one applies.
    const auto full = media_type_with_parameters(64);
    ruvia::http_accept_match range;
    range.update_media_type("text/*;q=0.3", full);
    range.update_media_type(full + ";q=0.9", full);
    EXPECT_EQ(range.quality(), 300);
}

TEST(Ruvia, http_accept_match_accumulates_fields_and_respects_specific_exclusion) {
    ruvia::http_accept_match media;
    media.update_media_type("text/*;q=0.8", "text/html");
    EXPECT_TRUE(media.matched());
    EXPECT_EQ(media.quality(), 800);
    media.update_media_type("text/html;q=0", "text/html");
    EXPECT_TRUE(!media.matched());
    EXPECT_EQ(media.quality(), 0);

    ruvia::http_accept_match language;
    language.update_token("en;q=0.5", "en-US", ruvia::http_accept_token_match_mode::language_prefix);
    EXPECT_TRUE(language.matched());
    EXPECT_EQ(language.quality(), 500);
}

TEST(Ruvia, http_accept_match_language_uses_longest_matching_range) {
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
        EXPECT_EQ(match.quality(), item.quality_);
        EXPECT_EQ(match.matched(), item.quality_ > 0);
    }
}

TEST(Ruvia, http_accept_match_language_combines_field_lines) {
    constexpr auto mode = ruvia::http_accept_token_match_mode::language_prefix;
    for (const bool specific_first : {false, true}) {
        ruvia::http_accept_match split;
        split.update_token(specific_first ? "de-DE;q=0" : "de;q=1", "de-DE-1996", mode);
        split.update_token(specific_first ? "de;q=1" : "de-DE;q=0", "de-DE-1996", mode);
        ruvia::http_accept_match joined;
        joined.update_token("de;q=1, de-DE;q=0", "de-DE-1996", mode);
        EXPECT_EQ(split.quality(), joined.quality());
        EXPECT_TRUE(!split.matched());
    }

    ruvia::http_accept_match exact;
    exact.update_token("de-DE;q=1", "de-DE-1996", ruvia::http_accept_token_match_mode::exact);
    EXPECT_TRUE(!exact.matched());
    exact.update_token("*;q=0.8", "de-DE-1996", ruvia::http_accept_token_match_mode::exact);
    EXPECT_EQ(exact.quality(), 800);
    exact.update_token("DE-de-1996;q=0.2", "de-DE-1996", ruvia::http_accept_token_match_mode::exact);
    EXPECT_EQ(exact.quality(), 200);
}

TEST(Ruvia, http_accept_match_token_fields_allow_only_an_optional_weight) {
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
            EXPECT_EQ(match.quality(), item.quality_);
            EXPECT_EQ(match.matched(), item.quality_ > 0);
        }
    }
}

TEST(Ruvia, http_accept_match_token_weights_accumulate_across_field_lines) {
    for (const auto mode : {ruvia::http_accept_token_match_mode::exact, ruvia::http_accept_token_match_mode::language_prefix}) {
        const auto offered = mode == ruvia::http_accept_token_match_mode::exact ? "en" : "en-US";
        for (const bool invalid_first : {false, true}) {
            ruvia::http_accept_match match;
            match.update_token(invalid_first ? "en;level=1" : "en;q=0.6", offered, mode);
            match.update_token(invalid_first ? "en;q=0.6" : "en;level=1", offered, mode);
            EXPECT_TRUE(match.matched());
            EXPECT_EQ(match.quality(), 600);
        }
    }
}

TEST(Ruvia, media_range_specificity_ordering) {
    // Full type/subtype overrides type/*, which overrides */*, even at lower q.
    constexpr std::string_view field = "*/*;q=0.9, text/*;q=0.7, text/html;q=0.2";
    ruvia::http_accept_match exact;
    exact.update_media_type(field, "text/html");
    EXPECT_EQ(exact.quality(), 200);
    ruvia::http_accept_match subtype_wildcard;
    subtype_wildcard.update_media_type(field, "text/plain");
    EXPECT_EQ(subtype_wildcard.quality(), 700);
    ruvia::http_accept_match wildcard;
    wildcard.update_media_type(field, "application/json");
    EXPECT_EQ(wildcard.quality(), 900);
}

TEST(Ruvia, http_accept_match_media_parameters_decide_specificity_across_fields) {
    constexpr std::string_view offered = "text/plain;charset=utf-8;format=flowed";
    constexpr std::string_view less_specific = "text/plain;format=flowed;q=0.8";
    constexpr std::string_view more_specific = "text/plain;CHARSET=\"UTF-8\";format=flowed;q=0.2";
    for (const bool specific_first : {false, true}) {
        ruvia::http_accept_match match;
        match.update_media_type(specific_first ? more_specific : less_specific, offered);
        match.update_media_type(specific_first ? less_specific : more_specific, offered);
        EXPECT_EQ(match.quality(), 200);
    }
    ruvia::http_accept_match joined;
    joined.update_media_type("text/plain;format=flowed;q=0.8, text/plain;CHARSET=\"UTF-8\";format=flowed;q=0.2", offered);
    EXPECT_EQ(joined.quality(), 200);

    // q is a weight, not an additional matching media parameter.
    for (const bool weighted_first : {false, true}) {
        ruvia::http_accept_match match;
        match.update_media_type(weighted_first ? "text/plain;q=0.9" : "text/plain", offered);
        match.update_media_type(weighted_first ? "text/plain" : "text/plain;q=0.9", offered);
        EXPECT_EQ(match.quality(), 1000);
    }
}

TEST(Ruvia, http_accept_match_media_invalid_parameter_suffix_preserves_the_best_match) {
    constexpr std::string_view offered = "text/plain;charset=utf-8;format=flowed";
    for (const std::string_view field : {
             "text/plain;format=flowed;FORMAT=flowed;q=1",
             "text/plain;format=flowed;charset=\"unterminated",
             "text/plain;format=flowed;charset;q=1",
             "text/plain;format=flowed;unknown=present;q=1"}) {
        ruvia::http_accept_match match;
        match.update_media_type("text/plain;q=0.5", offered);
        match.update_media_type(field, offered);
        EXPECT_EQ(match.quality(), 500);
    }
}

TEST(Ruvia, http_accept_match_media_parameter_lookup_handles_order_and_quoted_values) {
    constexpr std::string_view field = R"(text/plain;note="a;b=c";format="flowed";profile=aB;charset=utf-8;q=0.8)";
    for (const std::string_view offered : {
             R"(text/plain;FORMAT=flowed;note="a;b=c";profile="\a\B";CHARSET="UTF-8")",
             R"(text/plain;CHARSET="UTF-8";profile="\a\B";note="a;b=c";FORMAT=flowed)",
             R"(text/plain;profile=aB;note="a;b=c";charset=utf-8;format="flowed")"}) {
        ruvia::http_accept_match match;
        match.update_media_type(field, offered);
        EXPECT_EQ(match.quality(), 800);
    }
}

TEST(Ruvia, accepts_media_type_requires_a_valid_complete_offered_value) {
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
        EXPECT_TRUE(!match.matched());
        EXPECT_EQ(match.quality(), 0);
    }
}

TEST(Ruvia, http_accept_match_media_empty_parameter_slots_preserve_quality) {
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
            EXPECT_EQ(match.quality(), item.quality_);
            EXPECT_EQ(match.matched(), item.quality_ > 0);
        }
    }
}

TEST(Ruvia, http_accept_match_media_empty_slots_preserve_specificity_across_fields) {
    constexpr std::string_view general = "text/plain;;q=0.8;";
    constexpr std::string_view specific = "text/plain; ;charset=utf-8;;q=0;";
    for (const bool specific_first : {false, true}) {
        ruvia::http_accept_match excluded;
        excluded.update_media_type(specific_first ? specific : general, "text/plain;;charset=utf-8;");
        excluded.update_media_type(specific_first ? general : specific, "text/plain;;charset=utf-8;");
        EXPECT_TRUE(!excluded.matched());

        ruvia::http_accept_match other_charset;
        other_charset.update_media_type(specific_first ? specific : general, "text/plain;;charset=latin1;");
        other_charset.update_media_type(specific_first ? general : specific, "text/plain;;charset=latin1;");
        EXPECT_EQ(other_charset.quality(), 800);
    }
    for (const bool higher_first : {false, true}) {
        ruvia::http_accept_match repeated;
        repeated.update_media_type(higher_first ? "text/plain;;q=0.8;" : "text/plain;q=0.2", "text/plain;");
        repeated.update_media_type(higher_first ? "text/plain;q=0.2" : "text/plain;;q=0.8;", "text/plain;");
        EXPECT_EQ(repeated.quality(), 800);
    }
}

TEST(Ruvia, http_accept_match_keeps_quality_and_parameter_validation_consistent) {
    constexpr std::string_view offered = R"(text/plain;profile="a;b=c";charset=UTF-8)";
    struct media_case final {
        std::string_view field_;
        int quality_;
    };
    constexpr media_case cases[] = {
        {R"(text/plain;q=0.2;profile="a;b=c";charset=utf-8)", 200},
        {R"(text/plain;charset="utf-8";profile="a;b=c";q=0.3)", 300},
        {R"(text/plain;profile="a;b=c";q=0)", 0},
        {R"(text/plain;q=invalid;profile="a;b=c")", 0},
        {R"(text/plain;profile="a;b=c";q="0.5")", 0},
        {R"(text/plain;q=;profile="a;b=c")", 800},
        {R"(text/plain;q=0.5;Q=0.4;profile="a;b=c")", 800},
        {R"(text/plain;profile="a;b=c"suffix;q=0.5)", 800},
        {R"(text/plain;profile=a/b;q=0.5)", 800},
        {R"(text/plain;profile="a;b=c";charset =utf-8;q=0.5)", 800},
        {R"(text/plain;profile="a;b=c";charset= utf-8;q=0.5)", 800},
    };
    for (const auto& item : cases) {
        for (const bool specific_first : {false, true}) {
            ruvia::http_accept_match match;
            match.update_media_type(specific_first ? item.field_ : "text/*;q=0.8", offered);
            match.update_media_type(specific_first ? "text/*;q=0.8" : item.field_, offered);
            EXPECT_EQ(match.quality(), item.quality_);
        }
    }
}
