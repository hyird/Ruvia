#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <variant>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_priority.h"

TEST(Ruvia, http_priority_structured_dictionary_types_duplicates_and_parameters) {
    const auto parsed_value = ruvia::parse_http_priority("u=5;note=\"x\", i, extra=(token :YWJj:);p=?0");
    EXPECT_TRUE((parsed_value.index() == 0));
    EXPECT_EQ(*std::get<0>(parsed_value).urgency_, 5);
    EXPECT_TRUE(std::get<0>(parsed_value).incremental_ == true);
    const auto defaults = ruvia::parse_http_priority("u=8, i=42, u=2, u=\"ignored\"");
    EXPECT_TRUE((defaults.index() == 0) && !std::get<0>(defaults).urgency_ && !std::get<0>(defaults).incremental_);
    EXPECT_EQ(std::get<0>(defaults).request_priority().urgency_, 3);
    const auto partial = ruvia::parse_http_priority("u=0");
    EXPECT_TRUE((partial.index() == 0) && std::get<0>(partial).urgency_ == 0 && !std::get<0>(partial).incremental_);
    for (const auto invalid : {"u=", "u=2,", "U=2", "i=?2", "u=1.1234", "u=(x\t y)", "x=\"bad\\q\""}) {
        EXPECT_TRUE((ruvia::parse_http_priority(invalid).index() != 0));
    }
}
TEST(Ruvia, http_priority_byte_sequences_accept_optional_padding_and_validate_quartets) {
    for (const auto value : {"u=2, i;bytes=:YQ=:", "u=2, i;bytes=:YQ==:", "u=2, i;bytes=:YQ:",
             "u=2, i;bytes=:YWJj:", "u=2, i;bytes=:YWJ:", "u=2, i;bytes=:YWJ=:", "u=2, i;bytes=:YR==:",
             "u=2, i, x=:YQ=:", "u=2, i, x=(:YQ=:)", "u=2, i;bytes=::"}) {
        const auto parsed_value = ruvia::parse_http_priority(value);
        EXPECT_TRUE((parsed_value.index() == 0));
        if ((parsed_value.index() == 0)) {
            EXPECT_TRUE(std::get<0>(parsed_value).urgency_ == 2);
            EXPECT_TRUE(std::get<0>(parsed_value).incremental_ == true);
        }
    }
    for (const auto value : {"u=2, i;bytes=:Y:", "u=2, i;bytes=:Y=:", "u=2, i;bytes=:YWJj=:",
             "u=2, i;bytes=:YWJ==:", "u=2, i;bytes=:YQ===:", "u=2, i;bytes=:Y=Q:",
             "u=2, i;bytes=:YQ$:", "u=2, i;bytes=:YQ\n:", "u=2, i;bytes=:YQ"}) {
        EXPECT_TRUE((ruvia::parse_http_priority(value).index() != 0));
    }
}

TEST(Ruvia, http_priority_repeated_headers_combine_members_and_replace_invalid_values) {
    const std::array headers{ruvia::http_header_view{"Priority", "u=2, i"},
        ruvia::http_header_view{"X-Other", "u=0"},
        ruvia::http_header_view{"pRIORITY", "u=\"ignored\""}};
    const auto parsed_value = ruvia::parse_http_priority(headers);
    EXPECT_TRUE((parsed_value.index() == 0) && !std::get<0>(parsed_value).urgency_ && std::get<0>(parsed_value).incremental_ == true);
    const std::array malformed{ruvia::http_header_view{"priority", "u=2"},
        ruvia::http_header_view{"priority", "i=?2"}};
    EXPECT_TRUE((ruvia::parse_http_priority(malformed).index() != 0));
    const std::array empty_repeated{ruvia::http_header_view{"priority", ""},
        ruvia::http_header_view{"priority", "u=1"}};
    EXPECT_TRUE((ruvia::parse_http_priority(empty_repeated).index() != 0));
    EXPECT_TRUE(std::get<0>(ruvia::parse_http_priority(std::span<const ruvia::http_header_view>{})).request_priority().urgency_ == 3);
}

TEST(Ruvia, http_priority_repeated_fields_match_their_comma_joined_dictionary) {
    const std::array headers{ruvia::http_header_view{"Priority", "extra=\"alpha"},
        ruvia::http_header_view{"X-Other", "ignored"},
        ruvia::http_header_view{"priority", "beta\", u=1, i"}};
    const auto combined = ruvia::parse_http_priority("extra=\"alpha, beta\", u=1, i");
    EXPECT_TRUE((combined.index() == 0) && std::get<0>(combined).urgency_ == 1 && std::get<0>(combined).incremental_ == true);
    const auto parsed_value = ruvia::parse_http_priority(headers);
    EXPECT_TRUE((parsed_value.index() == 0));
    if (parsed_value.index() == 0) {
        EXPECT_TRUE(std::get<0>(parsed_value).urgency_ == 1);
        EXPECT_TRUE(std::get<0>(parsed_value).incremental_ == true);
    }

    const std::array parameter_headers{ruvia::http_header_view{"priority", "i;note=\"a"},
        ruvia::http_header_view{"priority", "b\", u=0"}};
    const auto parameters = ruvia::parse_http_priority(parameter_headers);
    EXPECT_TRUE((parameters.index() == 0));
    if (parameters.index() == 0) {
        EXPECT_TRUE(std::get<0>(parameters).urgency_ == 0);
        EXPECT_TRUE(std::get<0>(parameters).incremental_ == true);
    }
}

TEST(Ruvia, http_priority_repeated_fields_preserve_separators_and_empty_values) {
    const std::array with_empty{ruvia::http_header_view{"Priority", "extra=\"alpha"},
        ruvia::http_header_view{"priority", ""},
        ruvia::http_header_view{"pRIORITY", "beta\", u=1, i"}};
    const auto parsed_value = ruvia::parse_http_priority(with_empty);
    EXPECT_TRUE((parsed_value.index() == 0) && std::get<0>(parsed_value).urgency_ == 1 && std::get<0>(parsed_value).incremental_ == true);

    const std::array tab_separated{ruvia::http_header_view{"priority", "u=0"},
        ruvia::http_header_view{"priority", "\ti"}};
    const auto tab = ruvia::parse_http_priority(tab_separated);
    EXPECT_TRUE((tab.index() == 0) && std::get<0>(tab).urgency_ == 0 && std::get<0>(tab).incremental_ == true);

    const std::array fragments{
        std::array<std::string_view, 2>{"u=1", ""},
        std::array<std::string_view, 2>{"u=1", "\t"},
        std::array<std::string_view, 2>{"u=1, i=?", "1"},
        std::array<std::string_view, 2>{"u=1, i, x=:YQ", ":"},
        std::array<std::string_view, 2>{"u=1", "2, i"},
        std::array<std::string_view, 2>{"x=\"a\\", "\"b\", u=1, i"},
    };
    for (const auto& values : fragments) {
        const std::array headers{ruvia::http_header_view{"priority", values[0]},
            ruvia::http_header_view{"priority", values[1]}};
        EXPECT_TRUE(ruvia::parse_http_priority(headers).index() != 0);
    }
}

TEST(Ruvia, http_priority_http2_frame_roundtrips_and_output_bounds) {
    std::array<char, 64> bytes_value{};
    auto h2 = ruvia::encode_http2_priority_update(bytes_value, 5, {.urgency_ = 1, .incremental_ = true});
    EXPECT_TRUE((h2.index() == 0));
    auto decoded = ruvia::decode_http2_priority_update(std::span(bytes_value).subspan(9, std::get<0>(h2) - 9));
    EXPECT_TRUE((decoded.index() == 0) && std::get<0>(decoded).element_id_ == 5 && !std::get<0>(decoded).push_ && std::get<0>(decoded).fields_.urgency_ == 1);
    EXPECT_TRUE((ruvia::encode_http_priority(std::span(bytes_value).first(1), {.urgency_ = 2}).index() != 0));
    EXPECT_TRUE((ruvia::encode_http2_priority_update(bytes_value, 0, {}).index() != 0));
}

TEST(Ruvia, http_priority_rejects_wide_local_urgency_without_replacing_an_existing_signal) {
    std::array<char, 64> bytes{};
    const auto rejects = [&](auto input) {
        try {
            (void)ruvia::encode_http_priority(bytes, {.urgency_ = input});
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    for (const auto input : std::array<std::int64_t, 7>{
             std::numeric_limits<std::int64_t>::min(), -256, -1, 8,
             256, 259, std::numeric_limits<std::int64_t>::max()}) {
        EXPECT_TRUE(rejects(input));
    }
    EXPECT_TRUE(rejects(std::numeric_limits<std::uint64_t>::max()));

    ruvia::http_priority_fields fields{.urgency_ = 2, .incremental_ = true};
    bool rejected = false;
    try {
        fields.urgency_ = 259;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    const auto encoded = ruvia::encode_http_priority(bytes, fields);
    EXPECT_TRUE(encoded.index() == 0);
    if (const auto* count = std::get_if<std::size_t>(&encoded)) {
        const auto reparsed = ruvia::parse_http_priority(std::string_view(bytes.data(), *count));
        EXPECT_TRUE(reparsed.index() == 0);
        if (const auto* retained = std::get_if<ruvia::http_priority_fields>(&reparsed)) {
            EXPECT_TRUE(retained->urgency_ == 2);
            EXPECT_TRUE(retained->incremental_ == true);
        }
    }
}

TEST(Ruvia, http_priority_ignores_out_of_range_peer_urgency_instead_of_narrowing_it) {
    // RFC 9218 section 4 requires ignoring successfully parsed values outside
    // the urgency domain, including one that replaces a valid earlier member.
    for (const auto value : {"u=-256", "u=-1", "u=8", "u=256", "u=259", "u=2, u=256"}) {
        const auto parsed = ruvia::parse_http_priority(value);
        EXPECT_TRUE(parsed.index() == 0);
        if (const auto* fields = std::get_if<ruvia::http_priority_fields>(&parsed)) {
            EXPECT_TRUE(!fields->urgency_.has_value());
            EXPECT_TRUE(fields->request_priority().urgency_ == 3);
        }
    }
    const auto maximum = ruvia::parse_http_priority("u=7, i=?0");
    EXPECT_TRUE(maximum.index() == 0);
    if (const auto* fields = std::get_if<ruvia::http_priority_fields>(&maximum)) {
        EXPECT_TRUE(fields->urgency_ == 7);
        EXPECT_TRUE(fields->incremental_ == false);
    }
}
