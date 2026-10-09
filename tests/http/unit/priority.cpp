#include <array>
#include <string_view>
#include <variant>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpPriority.h"

#include "test_harness.h"

RUVIA_TEST(http_priority_structured_dictionary_types_duplicates_and_parameters) {
    const auto parsed = ruvia::parseHttpPriority("u=5;note=\"x\", i, extra=(token :YWJj:);p=?0");
    RUVIA_CHECK((parsed.index() == 0));
    RUVIA_CHECK_EQ(*std::get<0>(parsed).urgency, 5);
    RUVIA_CHECK(std::get<0>(parsed).incremental == true);
    const auto defaults = ruvia::parseHttpPriority("u=8, i=42, u=2, u=\"ignored\"");
    RUVIA_CHECK((defaults.index() == 0) && !std::get<0>(defaults).urgency && !std::get<0>(defaults).incremental);
    RUVIA_CHECK_EQ(std::get<0>(defaults).requestPriority().urgency, 3);
    const auto partial = ruvia::parseHttpPriority("u=0");
    RUVIA_CHECK((partial.index() == 0) && std::get<0>(partial).urgency == 0 && !std::get<0>(partial).incremental);
    for (const auto invalid : {"u=", "u=2,", "U=2", "i=?2", "u=1.1234", "u=(x\t y)", "x=\"bad\\q\""}) {
        RUVIA_CHECK((ruvia::parseHttpPriority(invalid).index() != 0));
    }
}
RUVIA_TEST(http_priority_byte_sequences_accept_optional_padding_and_validate_quartets) {
    for (const auto value : {"u=2, i;bytes=:YQ=:", "u=2, i;bytes=:YQ==:", "u=2, i;bytes=:YQ:",
             "u=2, i;bytes=:YWJj:", "u=2, i;bytes=:YWJ:", "u=2, i;bytes=:YWJ=:", "u=2, i;bytes=:YR==:",
             "u=2, i, x=:YQ=:", "u=2, i, x=(:YQ=:)", "u=2, i;bytes=::"}) {
        const auto parsed = ruvia::parseHttpPriority(value);
        RUVIA_CHECK((parsed.index() == 0));
        if ((parsed.index() == 0)) {
            RUVIA_CHECK(std::get<0>(parsed).urgency == 2);
            RUVIA_CHECK(std::get<0>(parsed).incremental == true);
        }
    }
    for (const auto value : {"u=2, i;bytes=:Y:", "u=2, i;bytes=:Y=:", "u=2, i;bytes=:YWJj=:",
             "u=2, i;bytes=:YWJ==:", "u=2, i;bytes=:YQ===:", "u=2, i;bytes=:Y=Q:",
             "u=2, i;bytes=:YQ$:", "u=2, i;bytes=:YQ\n:", "u=2, i;bytes=:YQ"}) {
        RUVIA_CHECK((ruvia::parseHttpPriority(value).index() != 0));
    }
}

RUVIA_TEST(http_priority_repeated_headers_combine_members_and_replace_invalid_values) {
    const std::array headers{ruvia::HttpHeaderView{"Priority", "u=2, i"},
        ruvia::HttpHeaderView{"X-Other", "u=0"},
        ruvia::HttpHeaderView{"pRIORITY", "u=\"ignored\""}};
    const auto parsed = ruvia::parseHttpPriority(headers);
    RUVIA_CHECK((parsed.index() == 0) && !std::get<0>(parsed).urgency && std::get<0>(parsed).incremental == true);
    const std::array malformed{ruvia::HttpHeaderView{"priority", "u=2"},
        ruvia::HttpHeaderView{"priority", "i=?2"}};
    RUVIA_CHECK((ruvia::parseHttpPriority(malformed).index() != 0));
    const std::array emptyRepeated{ruvia::HttpHeaderView{"priority", ""},
        ruvia::HttpHeaderView{"priority", "u=1"}};
    RUVIA_CHECK((ruvia::parseHttpPriority(emptyRepeated).index() != 0));
    RUVIA_CHECK(std::get<0>(ruvia::parseHttpPriority(std::span<const ruvia::HttpHeaderView>{})).requestPriority().urgency == 3);
}

RUVIA_TEST(http_priority_repeated_fields_match_their_comma_joined_dictionary) {
    const std::array headers{ruvia::HttpHeaderView{"Priority", "extra=\"alpha"},
        ruvia::HttpHeaderView{"X-Other", "ignored"},
        ruvia::HttpHeaderView{"priority", "beta\", u=1, i"}};
    const auto combined = ruvia::parseHttpPriority("extra=\"alpha, beta\", u=1, i");
    RUVIA_CHECK((combined.index() == 0) && std::get<0>(combined).urgency == 1 && std::get<0>(combined).incremental == true);
    const auto parsed = ruvia::parseHttpPriority(headers);
    RUVIA_CHECK((parsed.index() == 0));
    if (parsed.index() == 0) {
        RUVIA_CHECK(std::get<0>(parsed).urgency == 1);
        RUVIA_CHECK(std::get<0>(parsed).incremental == true);
    }

    const std::array parameter_headers{ruvia::HttpHeaderView{"priority", "i;note=\"a"},
        ruvia::HttpHeaderView{"priority", "b\", u=0"}};
    const auto parameters = ruvia::parseHttpPriority(parameter_headers);
    RUVIA_CHECK((parameters.index() == 0));
    if (parameters.index() == 0) {
        RUVIA_CHECK(std::get<0>(parameters).urgency == 0);
        RUVIA_CHECK(std::get<0>(parameters).incremental == true);
    }
}

RUVIA_TEST(http_priority_repeated_fields_preserve_separators_and_empty_values) {
    const std::array with_empty{ruvia::HttpHeaderView{"Priority", "extra=\"alpha"},
        ruvia::HttpHeaderView{"priority", ""},
        ruvia::HttpHeaderView{"pRIORITY", "beta\", u=1, i"}};
    const auto parsed = ruvia::parseHttpPriority(with_empty);
    RUVIA_CHECK((parsed.index() == 0) && std::get<0>(parsed).urgency == 1 && std::get<0>(parsed).incremental == true);

    const std::array tab_separated{ruvia::HttpHeaderView{"priority", "u=0"},
        ruvia::HttpHeaderView{"priority", "\ti"}};
    const auto tab = ruvia::parseHttpPriority(tab_separated);
    RUVIA_CHECK((tab.index() == 0) && std::get<0>(tab).urgency == 0 && std::get<0>(tab).incremental == true);

    const std::array fragments{
        std::array<std::string_view, 2>{"u=1", ""},
        std::array<std::string_view, 2>{"u=1", "\t"},
        std::array<std::string_view, 2>{"u=1, i=?", "1"},
        std::array<std::string_view, 2>{"u=1, i, x=:YQ", ":"},
        std::array<std::string_view, 2>{"u=1", "2, i"},
        std::array<std::string_view, 2>{"x=\"a\\", "\"b\", u=1, i"},
    };
    for (const auto& values : fragments) {
        const std::array headers{ruvia::HttpHeaderView{"priority", values[0]},
            ruvia::HttpHeaderView{"priority", values[1]}};
        RUVIA_CHECK(ruvia::parseHttpPriority(headers).index() != 0);
    }
}

RUVIA_TEST(http_priority_http2_http3_frame_roundtrips_and_output_bounds) {
    std::array<char, 64> bytes{};
    auto h2 = ruvia::encodeHttp2PriorityUpdate(bytes, 5, {.urgency = 1, .incremental = true});
    RUVIA_CHECK((h2.index() == 0));
    auto decoded = ruvia::decodeHttp2PriorityUpdate(std::span(bytes).subspan(9, std::get<0>(h2) - 9));
    RUVIA_CHECK((decoded.index() == 0) && std::get<0>(decoded).elementId == 5 && !std::get<0>(decoded).push && std::get<0>(decoded).fields.urgency == 1);
    for (const bool push : {false, true}) {
        const auto h3 = ruvia::encodeHttp3PriorityUpdate(bytes, {.elementId = push ? 9u : 12u, .push = push, .fields = {.incremental = false}});
        RUVIA_CHECK((h3.index() == 0));
        const auto frame = ruvia::decodeHttp3FrameHeader(std::span(bytes).first(std::get<0>(h3)));
        RUVIA_CHECK((frame.index() == 0));
        auto value = ruvia::decodeHttp3PriorityUpdate(std::get<0>(frame).type, std::span(bytes).subspan(std::get<0>(frame).encodedBytes, std::get<0>(h3) - std::get<0>(frame).encodedBytes));
        RUVIA_CHECK((value.index() == 0) && std::get<0>(value).push == push && std::get<0>(value).fields.incremental == false);
    }
    RUVIA_CHECK((ruvia::encodeHttpPriority(std::span(bytes).first(1), {.urgency = 2}).index() != 0));
    RUVIA_CHECK((ruvia::encodeHttp2PriorityUpdate(bytes, 0, {}).index() != 0));
}
