#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

#include "test_exceptions.h"

namespace {

std::string media_type_with_parameters(int count) {
    std::string value = "text/plain";
    for (int index = 0; index < count; ++index) {
        value += std::string(";p") + std::to_string(index).c_str() + "=\"a;b\"";
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

TEST(Ruvia, http_accept_match_plain_text_parameters_ignore_value_case) {
    struct media_case final {
        std::string_view field_;
        std::string_view offered_;
        int quality_;
    };
    constexpr media_case cases[] = {
        {"text/plain;format=Flowed;q=0.7", "text/plain;format=flowed", 700},
        {"text/plain;format=fixed;q=0.4", "TEXT/PLAIN;FORMAT=FIXED", 400},
        {"text/plain;delsp=Yes;q=0.6", "text/plain;format=flowed;delsp=yes", 600},
        {R"(text/plain;format="\F\L\O\W\E\D";delsp="\N\O";q=0.5)",
            "text/plain;format=flowed;delsp=no", 500},
        {"text/plain;format=flowed;delsp=yes;q=0.3",
            R"(text/plain;format="\F\L\O\W\E\D";DELSP="\Y\E\S")", 300},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.field_);
        ruvia::http_accept_match match;
        match.update_media_type(item.field_, item.offered_);
        EXPECT_EQ(match.quality(), item.quality_);
    }
}

TEST(Ruvia, http_accept_match_plain_text_parameter_exclusions_survive_field_order) {
    constexpr std::string_view offered = "text/plain;format=flowed;delsp=yes";
    constexpr std::string_view general = "*/*;q=0.8";
    for (const std::string_view excluded : {
             "text/plain;format=FLOWED;q=0",
             "text/*;delsp=YES;q=0",
             "*/*;format=FLOWED;delsp=YES;q=0"}) {
        SCOPED_TRACE(excluded);
        for (const bool exclusion_first : {false, true}) {
            ruvia::http_accept_match match;
            match.update_media_type(exclusion_first ? excluded : general, offered);
            match.update_media_type(exclusion_first ? general : excluded, offered);
            EXPECT_EQ(match.quality(), 0);
            EXPECT_FALSE(match.matched());
        }
        ruvia::http_accept_match joined;
        joined.update_media_type(std::string(general) + ", " + std::string(excluded), offered);
        EXPECT_FALSE(joined.matched());
    }
}

TEST(Ruvia, http_accept_match_preserves_other_parameter_value_case) {
    struct media_case final {
        std::string_view field_;
        std::string_view offered_;
    };
    constexpr media_case cases[] = {
        {"application/plain;format=Flowed", "application/plain;format=flowed"},
        {"text/example;format=Flowed", "text/example;format=flowed"},
        {"application/plain;delsp=Yes", "application/plain;delsp=yes"},
        {"text/example;delsp=Yes", "text/example;delsp=yes"},
        {"text/plain;profile=Example", "text/plain;profile=example"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.field_);
        ruvia::http_accept_match match;
        match.update_media_type(item.field_, item.offered_);
        EXPECT_FALSE(match.matched());
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

namespace {

using ruvia::http_byte_range_set;
using ruvia::resolve_http_byte_range_set;

[[nodiscard]] bool is_ignored_range(std::string_view value, std::uint64_t representation_length) {
    return resolve_http_byte_range_set(value, representation_length).ignored();
}

[[nodiscard]] bool is_unsatisfiable_range(
    std::string_view value, std::uint64_t representation_length) {
    return resolve_http_byte_range_set(value, representation_length).unsatisfiable();
}

}  // namespace

TEST(Ruvia, byte_range_set_discriminates_outcomes) {
    const auto resolved = resolve_http_byte_range_set("bytes=10-19", 100);
    EXPECT_TRUE(!resolved.ignored());
    EXPECT_TRUE(!resolved.unsatisfiable());
    EXPECT_EQ(resolved.size(), std::size_t{1});
    if (resolved.size() == 1) {
        const auto range = resolved[0];
        EXPECT_EQ(range.offset_, std::uint64_t{10});
        EXPECT_EQ(range.length_, std::uint64_t{10});
    }

    const auto ignored = resolve_http_byte_range_set("items=10-19", 100);
    EXPECT_TRUE(ignored.ignored());
    EXPECT_EQ(ignored.size(), std::size_t{0});

    const auto unsatisfiable = resolve_http_byte_range_set("bytes=100-", 100);
    EXPECT_TRUE(!unsatisfiable.ignored());
    EXPECT_TRUE(unsatisfiable.unsatisfiable());
    EXPECT_EQ(unsatisfiable.size(), std::size_t{0});
}

TEST(Ruvia, byte_range_set_bounded_and_open_ended) {
    const auto bounded = resolve_http_byte_range_set("bytes=100-199", 1000);
    EXPECT_EQ(bounded.size(), std::size_t{1});
    if (bounded.size() == 1) {
        const auto range = bounded[0];
        EXPECT_EQ(range.offset_, std::uint64_t{100});
        EXPECT_EQ(range.length_, std::uint64_t{100});
    }

    const auto open = resolve_http_byte_range_set("bytes=500-", 1000);
    EXPECT_EQ(open.size(), std::size_t{1});
    if (open.size() == 1) {
        const auto range = open[0];
        EXPECT_EQ(range.offset_, std::uint64_t{500});
        EXPECT_EQ(range.length_, std::uint64_t{500});
    }

    const auto last_byte = resolve_http_byte_range_set("bytes=999-999", 1000);
    EXPECT_EQ(last_byte.size(), std::size_t{1});
    if (last_byte.size() == 1) {
        EXPECT_EQ(last_byte[0].offset_, std::uint64_t{999});
        EXPECT_EQ(last_byte[0].length_, std::uint64_t{1});
    }

    const auto clamped_end = resolve_http_byte_range_set("bytes=0-2000", 1000);
    EXPECT_EQ(clamped_end.size(), std::size_t{1});
    if (clamped_end.size() == 1) {
        EXPECT_EQ(clamped_end[0].offset_, std::uint64_t{0});
        EXPECT_EQ(clamped_end[0].length_, std::uint64_t{1000});
    }
}

TEST(Ruvia, byte_range_set_suffix) {
    const auto suffix = resolve_http_byte_range_set("bytes=-100", 1000);
    EXPECT_EQ(suffix.size(), std::size_t{1});
    if (suffix.size() == 1) {
        EXPECT_EQ(suffix[0].offset_, std::uint64_t{900});
        EXPECT_EQ(suffix[0].length_, std::uint64_t{100});
    }

    const auto whole = resolve_http_byte_range_set("bytes=-2000", 1000);
    EXPECT_EQ(whole.size(), std::size_t{1});
    if (whole.size() == 1) {
        EXPECT_EQ(whole[0].offset_, std::uint64_t{0});
        EXPECT_EQ(whole[0].length_, std::uint64_t{1000});
    }
}

TEST(Ruvia, byte_range_set_unsatisfiable_is_payload_free) {
    EXPECT_TRUE(is_unsatisfiable_range("bytes=1000-", 1000));
    EXPECT_TRUE(is_unsatisfiable_range("bytes=1500-1600", 1000));
    EXPECT_TRUE(is_unsatisfiable_range("bytes=-0", 1000));
}

TEST(Ruvia, byte_range_set_ignores_invalid_and_unknown_values) {
    EXPECT_TRUE(is_ignored_range("items=0-99", 1000));
    EXPECT_TRUE(is_ignored_range("0-99", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=500-100", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=abc", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=x-9", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=0-x", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=-x", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=-", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=", 1000));
}

TEST(Ruvia, byte_range_set_supports_multiple_ranges) {
    const auto ranges = resolve_http_byte_range_set("bytes=0-99,200-299", 1000);
    EXPECT_TRUE(!ranges.ignored());
    EXPECT_TRUE(!ranges.unsatisfiable());
    EXPECT_EQ(ranges.size(), std::size_t{2});
    if (ranges.size() == 2) {
        EXPECT_EQ(ranges[0].offset_, std::uint64_t{0});
        EXPECT_EQ(ranges[0].length_, std::uint64_t{100});
        EXPECT_EQ(ranges[1].offset_, std::uint64_t{200});
        EXPECT_EQ(ranges[1].length_, std::uint64_t{100});
    }
}

TEST(Ruvia, byte_range_set_unit_is_case_insensitive) {
    const auto title_case = resolve_http_byte_range_set("Bytes=0-9", 100);
    const auto upper_case = resolve_http_byte_range_set("BYTES=90-", 100);
    EXPECT_EQ(title_case.size(), std::size_t{1});
    EXPECT_EQ(upper_case.size(), std::size_t{1});
    if (title_case.size() == 1) {
        EXPECT_EQ(title_case[0].offset_, std::uint64_t{0});
        EXPECT_EQ(title_case[0].length_, std::uint64_t{10});
    }
    if (upper_case.size() == 1) {
        EXPECT_EQ(upper_case[0].offset_, std::uint64_t{90});
        EXPECT_EQ(upper_case[0].length_, std::uint64_t{10});
    }
}

TEST(Ruvia, byte_range_set_allows_ows_after_equals) {
    const auto spaced = resolve_http_byte_range_set("bytes= \t10-19", 100);
    EXPECT_EQ(spaced.size(), std::size_t{1});
    if (spaced.size() == 1) {
        EXPECT_EQ(spaced[0].offset_, std::uint64_t{10});
        EXPECT_EQ(spaced[0].length_, std::uint64_t{10});
    }
}

TEST(Ruvia, byte_range_set_trims_field_value_ows) {
    const auto trailing = resolve_http_byte_range_set("bytes=10-19 \t", 100);
    const auto wrapped = resolve_http_byte_range_set(" \tbytes=20-29\t ", 100);
    EXPECT_EQ(trailing.size(), std::size_t{1});
    EXPECT_EQ(wrapped.size(), std::size_t{1});
    if (trailing.size() == 1) {
        EXPECT_EQ(trailing[0].offset_, std::uint64_t{10});
        EXPECT_EQ(trailing[0].length_, std::uint64_t{10});
    }
    if (wrapped.size() == 1) {
        EXPECT_EQ(wrapped[0].offset_, std::uint64_t{20});
        EXPECT_EQ(wrapped[0].length_, std::uint64_t{10});
    }
}

TEST(Ruvia, byte_range_set_huge_decimal_numerals_preserve_semantics) {
    const auto huge_start = resolve_http_byte_range_set("bytes=184467440737095516160-", 1000);
    EXPECT_TRUE(huge_start.unsatisfiable());

    const auto huge_end = resolve_http_byte_range_set("bytes=0-184467440737095516160", 1000);
    EXPECT_EQ(huge_end.size(), std::size_t{1});
    if (huge_end.size() == 1) {
        EXPECT_EQ(huge_end[0].offset_, std::uint64_t{0});
        EXPECT_EQ(huge_end[0].length_, std::uint64_t{1000});
    }

    const auto huge_suffix = resolve_http_byte_range_set("bytes=-184467440737095516160", 1000);
    EXPECT_EQ(huge_suffix.size(), std::size_t{1});
    if (huge_suffix.size() == 1) {
        EXPECT_EQ(huge_suffix[0].offset_, std::uint64_t{0});
        EXPECT_EQ(huge_suffix[0].length_, std::uint64_t{1000});
    }

    EXPECT_TRUE(is_ignored_range("bytes=184467440737095516160x-", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=184467440737095516160-184467440737095516159", 1000));
}

TEST(Ruvia, byte_range_set_preserves_disjoint_order_and_coalesces_touching_ranges) {
    const auto ranges = resolve_http_byte_range_set("bytes=50-59,0-9,20-29,90-", 100);
    EXPECT_EQ(ranges.size(), std::size_t{4});
    if (ranges.size() == 4) {
        EXPECT_EQ(ranges[0].offset_, std::uint64_t{50});
        EXPECT_EQ(ranges[1].offset_, std::uint64_t{0});
        EXPECT_EQ(ranges[2].offset_, std::uint64_t{20});
        EXPECT_EQ(ranges[3].offset_, std::uint64_t{90});
    }
    // A coalesced part takes the place of its earliest member, whatever order
    // its members were received in.
    const auto coalesced = resolve_http_byte_range_set("bytes=20-29,0-9,10-19,18-22,90-", 100);
    EXPECT_TRUE(!coalesced.ignored());
    EXPECT_TRUE(!coalesced.unsatisfiable());
    EXPECT_EQ(coalesced.size(), std::size_t{2});
    if (coalesced.size() == 2) {
        EXPECT_EQ(coalesced[0].offset_, std::uint64_t{0});
        EXPECT_EQ(coalesced[0].length_, std::uint64_t{30});
        EXPECT_EQ(coalesced[1].offset_, std::uint64_t{90});
        EXPECT_EQ(coalesced[1].length_, std::uint64_t{10});
    }
    const auto partial = resolve_http_byte_range_set("bytes=0-1,900-", 100);
    EXPECT_EQ(partial.size(), std::size_t{1});
    const auto none = resolve_http_byte_range_set("bytes=100-,200-", 100);
    EXPECT_TRUE(none.unsatisfiable());
}

TEST(Ruvia, byte_range_set_never_repeats_representation_bytes) {
    // Descending open-ended and suffix ranges each overlap every earlier one;
    // as separate parts they would multiply the response size.
    std::string open_ended = "bytes=";
    std::string suffixes = "bytes=";
    for (std::size_t index = 0; index < http_byte_range_set::capacity; ++index) {
        if (index != 0) {
            open_ended.push_back(',');
            suffixes.push_back(',');
        }
        open_ended.append(std::to_string(http_byte_range_set::capacity - 1 - index) + "-");
        suffixes.append(std::string("-") + std::to_string(1000 - index).c_str());
    }
    for (const auto& field : {open_ended, suffixes}) {
        const auto ranges = resolve_http_byte_range_set(field, 1000);
        EXPECT_EQ(ranges.size(), std::size_t{1});
        if (ranges.size() == 1) {
            EXPECT_EQ(ranges[0].offset_, std::uint64_t{0});
            EXPECT_EQ(ranges[0].length_, std::uint64_t{1000});
        }
    }

    // Ranges that only touch once a later range bridges them become one part.
    const auto bridged = resolve_http_byte_range_set("bytes=30-39,0-9,50-59,5-35", 100);
    EXPECT_EQ(bridged.size(), std::size_t{2});
    if (bridged.size() == 2) {
        EXPECT_EQ(bridged[0].offset_, std::uint64_t{0});
        EXPECT_EQ(bridged[0].length_, std::uint64_t{40});
        EXPECT_EQ(bridged[1].offset_, std::uint64_t{50});
        EXPECT_EQ(bridged[1].length_, std::uint64_t{10});
    }
}

TEST(Ruvia, byte_range_set_tolerates_bounded_empty_members) {
    const auto ranges = resolve_http_byte_range_set("bytes=,0-1,,2-3,", 100);
    EXPECT_EQ(ranges.size(), std::size_t{1});
    if (ranges.size() == 1) {
        EXPECT_EQ(ranges[0].offset_, std::uint64_t{0});
        EXPECT_EQ(ranges[0].length_, std::uint64_t{4});
    }
    EXPECT_TRUE(resolve_http_byte_range_set("bytes=,,", 100).ignored());
    EXPECT_EQ(resolve_http_byte_range_set("bytes=0-1,,3-4", 100).size(), std::size_t{2});
    EXPECT_TRUE(resolve_http_byte_range_set(
        "bytes=0-1,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,", 100)
            .ignored());
}

TEST(Ruvia, byte_range_set_counts_only_received_empty_members) {
    const auto prefix = std::string("bytes=") + std::string(32, ',');
    for (const auto tail : {"100-", "100-199", "-0"}) {
        const auto ranges = resolve_http_byte_range_set(prefix + tail, 100);
        EXPECT_TRUE(!ranges.ignored());
        EXPECT_TRUE(ranges.unsatisfiable());
    }
    const auto satisfiable = resolve_http_byte_range_set(prefix + "0-1", 100);
    EXPECT_EQ(satisfiable.size(), std::size_t{1});
    if (satisfiable.size() == 1) {
        EXPECT_EQ(satisfiable[0].offset_, std::uint64_t{0});
        EXPECT_EQ(satisfiable[0].length_, std::uint64_t{2});
    }
    EXPECT_TRUE(resolve_http_byte_range_set(prefix + ",100-", 100).ignored());
    EXPECT_TRUE(resolve_http_byte_range_set(prefix + ",0-1", 100).ignored());
}

TEST(Ruvia, byte_range_set_ignores_malformed_and_over_limit_sets) {
    std::string ranges = "bytes=";
    for (std::size_t index = 0; index <= http_byte_range_set::capacity; ++index) {
        if (index != 0) {
            ranges.push_back(',');
        }
        ranges.append(std::to_string(index * 2));
        ranges.push_back('-');
        ranges.append(std::to_string(index * 2));
    }
    EXPECT_TRUE(resolve_http_byte_range_set(ranges, 100).ignored());
}

TEST(Ruvia, byte_range_set_empty_representation_validates_syntax_before_unsatisfiable) {
    EXPECT_TRUE(is_unsatisfiable_range("bytes=0-0", 0));
    EXPECT_TRUE(is_unsatisfiable_range("bytes=0-99", 0));
    EXPECT_TRUE(is_unsatisfiable_range("bytes=-5", 0));
    EXPECT_TRUE(is_unsatisfiable_range("bytes=-1", 0));
    EXPECT_TRUE(is_unsatisfiable_range("BYTES=0-", 0));

    EXPECT_TRUE(is_ignored_range("bytes=abc", 0));
    EXPECT_TRUE(is_ignored_range("bytes=5-2", 0));
    EXPECT_TRUE(is_ignored_range("bytes=", 0));
    EXPECT_TRUE(is_ignored_range("bytes=-", 0));
    EXPECT_TRUE(is_ignored_range("bytes=0-0, invalid", 0));

    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}}) {
        EXPECT_TRUE(ruvia::test::throws_on(
            [&] { (void)resolve_http_byte_range_set("bytes=0-1", invalid); }));
    }
}

TEST(Ruvia, byte_range_set_rejects_out_of_range_index) {
    const auto resolved = resolve_http_byte_range_set("bytes=0-9", 100);
    EXPECT_EQ(resolved.size(), std::size_t{1});
    const auto throws_out_of_range = [](auto&& access) {
        try {
            (void)access();
        } catch (const std::out_of_range&) {
            return true;
        }
        return false;
    };
    EXPECT_TRUE(!throws_out_of_range([&] { return resolved[0]; }));
    EXPECT_TRUE(throws_out_of_range([&] { return resolved[1]; }));
    EXPECT_TRUE(throws_out_of_range([&] { return resolved[http_byte_range_set::capacity]; }));
    const auto ignored = resolve_http_byte_range_set("items=0-9", 100);
    EXPECT_TRUE(throws_out_of_range([&] { return ignored[0]; }));
}

TEST(Ruvia, parse_cache_control_flags_and_ages) {
    const auto cc = ruvia::parse_cache_control(
        "public, max-age=60, s-maxage=120, stale-while-revalidate=30, immutable");
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::immutable));
    EXPECT_TRUE(!cc.has(static_cast<ruvia::cache_control_directive>(
        static_cast<std::uint16_t>(ruvia::cache_control_directive::public_value) |
        static_cast<std::uint16_t>(ruvia::cache_control_directive::immutable))));
    EXPECT_TRUE(cc.max_age().has_value());
    EXPECT_EQ(*cc.max_age(), std::uint64_t{60});
    EXPECT_EQ(*cc.s_maxage(), std::uint64_t{120});
    EXPECT_EQ(*cc.stale_while_revalidate(), std::uint64_t{30});
}

TEST(Ruvia, parse_cache_control_request_directives) {
    const auto request =
        ruvia::parse_cache_control("only-if-cached, max-age=0, min-fresh=15, max-stale=30");
    EXPECT_TRUE(request.has(ruvia::cache_control_directive::only_if_cached));
    EXPECT_EQ(request.max_age().value_or(1), std::uint64_t{0});
    EXPECT_EQ(request.min_fresh().value_or(0), std::uint64_t{15});
    EXPECT_EQ(request.max_stale().value_or(0), std::uint64_t{30});
    EXPECT_TRUE(!request.has(ruvia::cache_control_directive::max_stale_any));

    const auto any_stale = ruvia::parse_cache_control("max-stale");
    EXPECT_TRUE(any_stale.has(ruvia::cache_control_directive::max_stale_any));
    EXPECT_TRUE(!any_stale.max_stale().has_value());

    const auto invalid =
        ruvia::parse_cache_control("only-if-cached=yes, min-fresh=bad, max-stale=bad");
    EXPECT_TRUE(!invalid.has(ruvia::cache_control_directive::only_if_cached));
    EXPECT_TRUE(!invalid.min_fresh().has_value());
    EXPECT_TRUE(!invalid.max_stale().has_value());
    EXPECT_TRUE(!invalid.has(ruvia::cache_control_directive::max_stale_any));
}

TEST(Ruvia, parse_cache_control_request_freshness_uses_first_occurrence) {
    ruvia::cache_control_field_parser parser;
    parser.update("min-fresh=5, max-stale=10");
    parser.update("min-fresh=50, max-stale");
    const auto request = parser.finish();
    EXPECT_EQ(request.min_fresh().value_or(0), std::uint64_t{5});
    EXPECT_EQ(request.max_stale().value_or(0), std::uint64_t{10});
    EXPECT_TRUE(!request.has(ruvia::cache_control_directive::max_stale_any));
}

TEST(Ruvia, parse_cache_control_no_store_and_private) {
    const auto cc = ruvia::parse_cache_control("no-store, private, must-revalidate");
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::private_value));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::must_revalidate));
    EXPECT_TRUE(!cc.max_age().has_value());
}

TEST(Ruvia, parse_cache_control_no_transform_is_bare_and_quote_aware) {
    const auto present = ruvia::parse_cache_control("NO-TRANSFORM");
    EXPECT_TRUE(present.has(ruvia::cache_control_directive::no_transform));

    const auto quoted = ruvia::parse_cache_control(R"(extension="a, no-transform, b")");
    EXPECT_TRUE(!quoted.has(ruvia::cache_control_directive::no_transform));

    const auto qualified = ruvia::parse_cache_control("no-transform=ignored");
    EXPECT_TRUE(!qualified.has(ruvia::cache_control_directive::no_transform));
}

TEST(Ruvia, parse_cache_control_quoted_and_case_insensitive_and_unknown) {
    // Directive names are case-insensitive; a quoted delta-seconds is accepted; unknown ignored.
    const auto cc = ruvia::parse_cache_control("Max-Age=\"45\" , surrogate-control=foo, No-Cache");
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::no_cache));
    EXPECT_TRUE(cc.max_age().has_value());
    EXPECT_EQ(*cc.max_age(), std::uint64_t{45});
}

TEST(Ruvia, parse_cache_control_decodes_quoted_pairs_in_delta_seconds) {
    const auto cc = ruvia::parse_cache_control(
        R"(max-age="6\0", s-maxage="1\20", stale-while-revalidate="\30", stale-if-error="4\5")");
    EXPECT_EQ(cc.max_age().value_or(0), std::uint64_t{60});
    EXPECT_EQ(cc.s_maxage().value_or(0), std::uint64_t{120});
    EXPECT_EQ(cc.stale_while_revalidate().value_or(0), std::uint64_t{30});
    EXPECT_EQ(cc.stale_if_error().value_or(0), std::uint64_t{45});
}

TEST(Ruvia, parse_cache_control_rejects_bad_delta_seconds) {
    const auto cc = ruvia::parse_cache_control("max-age=abc, s-maxage=");
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_TRUE(!cc.s_maxage().has_value());
}

TEST(Ruvia, parse_cache_control_rejects_whitespace_around_equals) {
    const auto cc = ruvia::parse_cache_control(
        "max-age =60, s-maxage= 120, stale-while-revalidate = 30, "
        "stale-if-error=\t45, no-cache =\"Set-Cookie\", private = auth, "
        "no-cache= \"ETag\", private= auth");
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_TRUE(!cc.s_maxage().has_value());
    EXPECT_TRUE(!cc.stale_while_revalidate().has_value());
    EXPECT_TRUE(!cc.stale_if_error().has_value());
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_cache));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::private_value));

    // OWS around comma separators remains valid list framing.
    const auto separated = ruvia::parse_cache_control(" max-age=60 \t,\t no-store ");
    EXPECT_EQ(separated.max_age().value_or(0), std::uint64_t{60});
    EXPECT_TRUE(separated.has(ruvia::cache_control_directive::no_store));
}

TEST(Ruvia, parse_cache_control_freshness_uses_first_occurrence) {
    const auto duplicated = ruvia::parse_cache_control(
        "max-age=60, MAX-AGE=3600, s-maxage=120, s-maxage=7200, "
        "stale-while-revalidate=30, stale-while-revalidate=300, "
        "stale-if-error=45, stale-if-error=450");
    EXPECT_EQ(duplicated.max_age().value_or(0), std::uint64_t{60});
    EXPECT_EQ(duplicated.s_maxage().value_or(0), std::uint64_t{120});
    EXPECT_EQ(duplicated.stale_while_revalidate().value_or(0), std::uint64_t{30});
    EXPECT_EQ(duplicated.stale_if_error().value_or(0), std::uint64_t{45});

    // An invalid first occurrence cannot be repaired by a later value; caches
    // must not accidentally turn invalid freshness information into freshness.
    const auto invalid_first =
        ruvia::parse_cache_control("max-age=invalid, max-age=3600, s-maxage=, s-maxage=7200");
    EXPECT_TRUE(!invalid_first.max_age().has_value());
    EXPECT_TRUE(!invalid_first.s_maxage().has_value());
}

TEST(Ruvia, cache_control_field_parser_combines_repeated_lines) {
    ruvia::cache_control_field_parser parser;
    parser.update("public, max-age=invalid, s-maxage=120");
    parser.update(R"(extension="a, no-transform, b", no-transform, max-age=3600, s-maxage=7200)");

    const auto cc = parser.finish();
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::no_transform));
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_EQ(cc.s_maxage().value_or(0), std::uint64_t{120});
}

TEST(Ruvia, parse_cache_control_delta_seconds_overflow_saturates) {
    const auto cc = ruvia::parse_cache_control(
        "max-age=184467440737095516150, "
        "s-maxage=\"184467440737095516150\"");
    EXPECT_EQ(cc.max_age().value_or(0), (std::numeric_limits<std::uint64_t>::max)());
    EXPECT_EQ(cc.s_maxage().value_or(0), (std::numeric_limits<std::uint64_t>::max)());
}

TEST(Ruvia, parse_cache_control_does_not_split_quoted_extension_values) {
    const auto cc = ruvia::parse_cache_control("extension=\"a, public, max-age=999, b\", private");
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::private_value));

    // A quoted-pair keeps the escaped quote inside the extension value; its
    // commas likewise cannot introduce directives.
    const auto escaped = ruvia::parse_cache_control("extension=\"a\\\", no-store, b\", immutable");
    EXPECT_TRUE(!escaped.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(escaped.has(ruvia::cache_control_directive::immutable));

    // An unterminated quoted value is malformed through the end of the field,
    // so a comma within it must not accidentally enable caching directives.
    const auto unterminated = ruvia::parse_cache_control("extension=\"a, public, max-age=3600");
    EXPECT_TRUE(!unterminated.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(!unterminated.max_age().has_value());
}

TEST(Ruvia, parse_cache_control_ignores_arguments_on_bare_directives) {
    const auto cc = ruvia::parse_cache_control(
        "public=ignored, no-store=ignored, must-revalidate=x, "
        "proxy-revalidate=\"x\", no-transform=ignored, immutable=");
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::must_revalidate));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::proxy_revalidate));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_transform));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::immutable));

    // no-cache and private are different: their response forms can carry a
    // field-name list, so an argument does not invalidate the directive.
    const auto qualified =
        ruvia::parse_cache_control("no-cache=\"Set-Cookie\", private=\"Authorization\"");
    EXPECT_TRUE(qualified.has(ruvia::cache_control_directive::no_cache));
    EXPECT_TRUE(qualified.has(ruvia::cache_control_directive::private_value));
}

TEST(Ruvia, parse_http_date_imf_fixdate) {
    // RFC 7231 example: Sun, 06 Nov 1994 08:49:37 GMT.
    const auto t = ruvia::parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT");
    EXPECT_TRUE(t.has_value());
    EXPECT_EQ(static_cast<long long>(*t), 784111777LL);
    EXPECT_TRUE(!ruvia::parse_http_date("not a date").has_value());
}

TEST(Ruvia, format_http_vary_cache_key_distinguishes_absent_from_empty_header) {
    const auto vary = ruvia::parse_http_vary("Accept-Language, X-Mode");
    const std::array<ruvia::http_header_view, 1> absent{
        ruvia::http_header_view{"X-Mode", "fast"},
    };
    const std::array<ruvia::http_header_view, 2> empty_value{
        ruvia::http_header_view{"Accept-Language", ""},
        ruvia::http_header_view{"X-Mode", "fast"},
    };
    const std::array<ruvia::http_header_view, 2> ows_only{
        ruvia::http_header_view{"Accept-Language", " \t "},
        ruvia::http_header_view{"X-Mode", "fast"},
    };
    const auto absent_key = ruvia::format_http_vary_cache_key(vary, absent);
    const auto empty_key = ruvia::format_http_vary_cache_key(vary, empty_value);
    const auto ows_key = ruvia::format_http_vary_cache_key(vary, ows_only);
    ASSERT_TRUE(absent_key.has_value());
    ASSERT_TRUE(empty_key.has_value());
    ASSERT_TRUE(ows_key.has_value());
    EXPECT_NE(*absent_key, *empty_key);
    EXPECT_EQ(*empty_key, *ows_key);
    EXPECT_FALSE(ruvia::http_vary_requests_match(vary, absent, empty_value));
    EXPECT_TRUE(ruvia::http_vary_requests_match(vary, empty_value, ows_only));
}

// ETag comparison and IMF-fixdate parsing back the conditional-request handling
// (If-Match / If-None-Match / If-Range, RFC 9110) for static file responses.

// Date formatting, cache framing, and locale-independent IMF-fixdate coverage
// live in date_parsing.cpp. This file covers precondition evaluation.

TEST(Ruvia, conditional_method_plan_follows_precondition_and_range_semantics) {
    using ruvia::http_conditional_method_plan;

    const auto get = get_http_conditional_method_plan(ruvia::http_known_method::get);
    EXPECT_TRUE(get.evaluates_preconditions_);
    EXPECT_TRUE(get.uses_not_modified_response_);
    EXPECT_TRUE(get.evaluates_if_modified_since_);
    EXPECT_TRUE(get.evaluates_range_);

    const auto head = get_http_conditional_method_plan(ruvia::http_known_method::head);
    EXPECT_TRUE(head.evaluates_preconditions_);
    EXPECT_TRUE(head.uses_not_modified_response_);
    EXPECT_TRUE(head.evaluates_if_modified_since_);
    EXPECT_TRUE(!head.evaluates_range_);

    for (const auto method : {ruvia::http_known_method::post, ruvia::http_known_method::put,
             ruvia::http_known_method::delete_value, ruvia::http_known_method::patch}) {
        const auto plan = get_http_conditional_method_plan(method);
        EXPECT_TRUE(plan.evaluates_preconditions_);
        EXPECT_TRUE(!plan.uses_not_modified_response_);
        EXPECT_TRUE(!plan.evaluates_if_modified_since_);
        EXPECT_TRUE(!plan.evaluates_range_);
    }

    for (const auto method : {ruvia::http_known_method::options, ruvia::http_known_method::connect,
             ruvia::http_known_method::unknown}) {
        const auto plan = get_http_conditional_method_plan(method);
        EXPECT_TRUE(!plan.evaluates_preconditions_);
        EXPECT_TRUE(!plan.uses_not_modified_response_);
        EXPECT_TRUE(!plan.evaluates_if_modified_since_);
        EXPECT_TRUE(!plan.evaluates_range_);
    }
}

TEST(Ruvia, http_date_precondition_comparisons) {
    using ruvia::http_date_not_modified;
    using ruvia::http_date_unmodified;

    constexpr std::time_t canonical{784111777};
    const auto later = canonical + 1;
    const auto earlier = canonical - 1;
    constexpr auto header_value = "Sun, 06 Nov 1994 08:49:37 GMT";

    EXPECT_TRUE(http_date_not_modified(header_value, canonical));
    EXPECT_TRUE(http_date_not_modified(header_value, earlier));
    EXPECT_TRUE(!http_date_not_modified(header_value, later));
    EXPECT_TRUE(!http_date_not_modified("not-a-date", canonical));

    EXPECT_TRUE(http_date_unmodified(header_value, canonical));
    EXPECT_TRUE(http_date_unmodified(header_value, earlier));
    EXPECT_TRUE(!http_date_unmodified(header_value, later));
    EXPECT_TRUE(http_date_unmodified("not-a-date", later));
}

TEST(Ruvia, http_if_range_requires_exact_validator) {
    using ruvia::http_if_range_allows;

    constexpr std::time_t canonical{784111777};
    constexpr auto etag = R"("abc")";
    constexpr auto date = "Sun, 06 Nov 1994 08:49:37 GMT";

    EXPECT_TRUE(!http_if_range_allows("", etag, canonical, true));
    EXPECT_TRUE(http_if_range_allows(etag, etag, canonical, true));
    EXPECT_TRUE(!http_if_range_allows(R"(W/"abc")", etag, canonical, true));
    EXPECT_TRUE(http_if_range_allows(date, etag, canonical, true));
    EXPECT_TRUE(!http_if_range_allows(date, etag, canonical + 1, true));
    EXPECT_TRUE(!http_if_range_allows(date, etag, canonical, false));
    EXPECT_TRUE(!http_if_range_allows(date, etag, std::nullopt, true));
    EXPECT_TRUE(http_if_range_allows(etag, etag, std::nullopt, false));
}

TEST(Ruvia, http_etag_preconditions_fold_repeated_field_lines) {
    using ruvia::http_etag_preconditions;
    using ruvia::http_header_view;

    const http_header_view fields[]{{"If-None-Match", R"("current")"}, {"If-None-Match", R"("stale")"}};
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/", fields, {});

    const auto matched = get_http_etag_preconditions(request, R"("current")");
    EXPECT_TRUE(matched.if_none_match_.present_);
    EXPECT_TRUE(matched.if_none_match_.matches());

    const http_header_view wildcard_fields[]{{"If-Match", "*"}};
    const auto wildcard = ruvia::make_parsed_http_request(
        "GET", "/", wildcard_fields, {});
    const auto existence = get_http_etag_preconditions(wildcard, R"("unused")");
    EXPECT_TRUE(existence.if_match_.present_);
    EXPECT_TRUE(existence.if_match_.matches());
    EXPECT_TRUE(existence.if_match_.wildcard_);
}

namespace {

template <typename request_type>
concept conditional_headers_accepts_request = requires(request_type&& request) {
    ruvia::get_http_conditional_headers(std::forward<request_type>(request));
};

static_assert(conditional_headers_accepts_request<const ruvia::http_request&>);
static_assert(!conditional_headers_accepts_request<ruvia::http_request>);

}  // namespace
