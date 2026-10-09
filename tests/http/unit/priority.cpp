#include <array>
#include <string_view>
#include <variant>

#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_priority.h"

#include "test_harness.h"

RUVIA_TEST(http_priority_structured_dictionary_types_duplicates_and_parameters) {
    const auto parsed_value = ruvia::parse_http_priority("u=5;note=\"x\", i, extra=(token :YWJj:);p=?0");
    RUVIA_CHECK((parsed_value.index() == 0));
    RUVIA_CHECK_EQ(*std::get<0>(parsed_value).urgency_, 5);
    RUVIA_CHECK(std::get<0>(parsed_value).incremental_ == true);
    const auto defaults = ruvia::parse_http_priority("u=8, i=42, u=2, u=\"ignored\"");
    RUVIA_CHECK((defaults.index() == 0) && !std::get<0>(defaults).urgency_ && !std::get<0>(defaults).incremental_);
    RUVIA_CHECK_EQ(std::get<0>(defaults).request_priority().urgency_, 3);
    const auto partial = ruvia::parse_http_priority("u=0");
    RUVIA_CHECK((partial.index() == 0) && std::get<0>(partial).urgency_ == 0 && !std::get<0>(partial).incremental_);
    for (const auto invalid : {"u=", "u=2,", "U=2", "i=?2", "u=1.1234", "u=(x\t y)", "x=\"bad\\q\""}) {
        RUVIA_CHECK((ruvia::parse_http_priority(invalid).index() != 0));
    }
}
RUVIA_TEST(http_priority_byte_sequences_accept_optional_padding_and_validate_quartets) {
    for (const auto value : {"u=2, i;bytes=:YQ=:", "u=2, i;bytes=:YQ==:", "u=2, i;bytes=:YQ:",
             "u=2, i;bytes=:YWJj:", "u=2, i;bytes=:YWJ:", "u=2, i;bytes=:YWJ=:", "u=2, i;bytes=:YR==:",
             "u=2, i, x=:YQ=:", "u=2, i, x=(:YQ=:)", "u=2, i;bytes=::"}) {
        const auto parsed_value = ruvia::parse_http_priority(value);
        RUVIA_CHECK((parsed_value.index() == 0));
        if ((parsed_value.index() == 0)) {
            RUVIA_CHECK(std::get<0>(parsed_value).urgency_ == 2);
            RUVIA_CHECK(std::get<0>(parsed_value).incremental_ == true);
        }
    }
    for (const auto value : {"u=2, i;bytes=:Y:", "u=2, i;bytes=:Y=:", "u=2, i;bytes=:YWJj=:",
             "u=2, i;bytes=:YWJ==:", "u=2, i;bytes=:YQ===:", "u=2, i;bytes=:Y=Q:",
             "u=2, i;bytes=:YQ$:", "u=2, i;bytes=:YQ\n:", "u=2, i;bytes=:YQ"}) {
        RUVIA_CHECK((ruvia::parse_http_priority(value).index() != 0));
    }
}

RUVIA_TEST(http_priority_repeated_headers_combine_members_and_replace_invalid_values) {
    const std::array headers{ruvia::http_header_view{"Priority", "u=2, i"},
        ruvia::http_header_view{"X-Other", "u=0"},
        ruvia::http_header_view{"pRIORITY", "u=\"ignored\""}};
    const auto parsed_value = ruvia::parse_http_priority(headers);
    RUVIA_CHECK((parsed_value.index() == 0) && !std::get<0>(parsed_value).urgency_ && std::get<0>(parsed_value).incremental_ == true);
    const std::array malformed{ruvia::http_header_view{"priority", "u=2"},
        ruvia::http_header_view{"priority", "i=?2"}};
    RUVIA_CHECK((ruvia::parse_http_priority(malformed).index() != 0));
    const std::array empty_repeated{ruvia::http_header_view{"priority", ""},
        ruvia::http_header_view{"priority", "u=1"}};
    RUVIA_CHECK((ruvia::parse_http_priority(empty_repeated).index() != 0));
    RUVIA_CHECK(std::get<0>(ruvia::parse_http_priority(std::span<const ruvia::http_header_view>{})).request_priority().urgency_ == 3);
}

RUVIA_TEST(http_priority_repeated_fields_match_their_comma_joined_dictionary) {
    const std::array headers{ruvia::http_header_view{"Priority", "extra=\"alpha"},
        ruvia::http_header_view{"X-Other", "ignored"},
        ruvia::http_header_view{"priority", "beta\", u=1, i"}};
    const auto combined = ruvia::parse_http_priority("extra=\"alpha, beta\", u=1, i");
    RUVIA_CHECK((combined.index() == 0) && std::get<0>(combined).urgency_ == 1 && std::get<0>(combined).incremental_ == true);
    const auto parsed_value = ruvia::parse_http_priority(headers);
    RUVIA_CHECK((parsed_value.index() == 0));
    if (parsed_value.index() == 0) {
        RUVIA_CHECK(std::get<0>(parsed_value).urgency_ == 1);
        RUVIA_CHECK(std::get<0>(parsed_value).incremental_ == true);
    }

    const std::array parameter_headers{ruvia::http_header_view{"priority", "i;note=\"a"},
        ruvia::http_header_view{"priority", "b\", u=0"}};
    const auto parameters = ruvia::parse_http_priority(parameter_headers);
    RUVIA_CHECK((parameters.index() == 0));
    if (parameters.index() == 0) {
        RUVIA_CHECK(std::get<0>(parameters).urgency_ == 0);
        RUVIA_CHECK(std::get<0>(parameters).incremental_ == true);
    }
}

RUVIA_TEST(http_priority_repeated_fields_preserve_separators_and_empty_values) {
    const std::array with_empty{ruvia::http_header_view{"Priority", "extra=\"alpha"},
        ruvia::http_header_view{"priority", ""},
        ruvia::http_header_view{"pRIORITY", "beta\", u=1, i"}};
    const auto parsed_value = ruvia::parse_http_priority(with_empty);
    RUVIA_CHECK((parsed_value.index() == 0) && std::get<0>(parsed_value).urgency_ == 1 && std::get<0>(parsed_value).incremental_ == true);

    const std::array tab_separated{ruvia::http_header_view{"priority", "u=0"},
        ruvia::http_header_view{"priority", "\ti"}};
    const auto tab = ruvia::parse_http_priority(tab_separated);
    RUVIA_CHECK((tab.index() == 0) && std::get<0>(tab).urgency_ == 0 && std::get<0>(tab).incremental_ == true);

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
        RUVIA_CHECK(ruvia::parse_http_priority(headers).index() != 0);
    }
}

RUVIA_TEST(http_priority_http2_http3_frame_roundtrips_and_output_bounds) {
    std::array<char, 64> bytes_value{};
    auto h2 = ruvia::encode_http2_priority_update(bytes_value, 5, {.urgency_ = 1, .incremental_ = true});
    RUVIA_CHECK((h2.index() == 0));
    auto decoded = ruvia::decode_http2_priority_update(std::span(bytes_value).subspan(9, std::get<0>(h2) - 9));
    RUVIA_CHECK((decoded.index() == 0) && std::get<0>(decoded).element_id_ == 5 && !std::get<0>(decoded).push_ && std::get<0>(decoded).fields_.urgency_ == 1);
    for (const bool push : {false, true}) {
        const auto h3 = ruvia::encode_http3_priority_update(bytes_value, {.element_id_ = push ? 9u : 12u, .push_ = push, .fields_ = {.incremental_ = false}});
        RUVIA_CHECK((h3.index() == 0));
        const auto frame = ruvia::decode_http3_frame_header(std::span(bytes_value).first(std::get<0>(h3)));
        RUVIA_CHECK((frame.index() == 0));
        auto value = ruvia::decode_http3_priority_update(std::get<0>(frame).type_, std::span(bytes_value).subspan(std::get<0>(frame).encoded_bytes_, std::get<0>(h3) - std::get<0>(frame).encoded_bytes_));
        RUVIA_CHECK((value.index() == 0) && std::get<0>(value).push_ == push && std::get<0>(value).fields_.incremental_ == false);
    }
    RUVIA_CHECK((ruvia::encode_http_priority(std::span(bytes_value).first(1), {.urgency_ = 2}).index() != 0));
    RUVIA_CHECK((ruvia::encode_http2_priority_update(bytes_value, 0, {}).index() != 0));
}
