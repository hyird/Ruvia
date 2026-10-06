#include <array>
#include <string_view>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpPriority.h"

#include "test_harness.h"

RUVIA_TEST(http_priority_structured_dictionary_types_duplicates_and_parameters) {
    const auto parsed = ruvia::parseHttpPriority("u=5;note=\"x\", i, extra=(token :YWJj:);p=?0");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK_EQ(*parsed->urgency, 5);
    RUVIA_CHECK(parsed->incremental == true);
    const auto defaults = ruvia::parseHttpPriority("u=8, i=42, u=2, u=\"ignored\"");
    RUVIA_CHECK(defaults && !defaults->urgency && !defaults->incremental);
    RUVIA_CHECK_EQ(defaults->requestPriority().urgency, 3);
    const auto partial = ruvia::parseHttpPriority("u=0");
    RUVIA_CHECK(partial && partial->urgency == 0 && !partial->incremental);
    for (const auto invalid : {"u=", "u=2,", "U=2", "i=?2", "u=1.1234", "u=(x\t y)", "x=\"bad\\q\""}) {
        RUVIA_CHECK(!ruvia::parseHttpPriority(invalid));
    }
}
RUVIA_TEST(http_priority_byte_sequences_accept_optional_padding_and_validate_quartets) {
    for (const auto value : {"u=2, i;bytes=:YQ=:", "u=2, i;bytes=:YQ==:", "u=2, i;bytes=:YQ:",
             "u=2, i;bytes=:YWJj:", "u=2, i;bytes=:YWJ:", "u=2, i;bytes=:YWJ=:", "u=2, i;bytes=:YR==:",
             "u=2, i, x=:YQ=:", "u=2, i, x=(:YQ=:)", "u=2, i;bytes=::"}) {
        const auto parsed = ruvia::parseHttpPriority(value);
        RUVIA_CHECK(parsed.has_value());
        if (parsed) {
            RUVIA_CHECK(parsed->urgency == 2);
            RUVIA_CHECK(parsed->incremental == true);
        }
    }
    for (const auto value : {"u=2, i;bytes=:Y:", "u=2, i;bytes=:Y=:", "u=2, i;bytes=:YWJj=:",
             "u=2, i;bytes=:YWJ==:", "u=2, i;bytes=:YQ===:", "u=2, i;bytes=:Y=Q:",
             "u=2, i;bytes=:YQ$:", "u=2, i;bytes=:YQ\n:", "u=2, i;bytes=:YQ"}) {
        RUVIA_CHECK(!ruvia::parseHttpPriority(value));
    }
}

RUVIA_TEST(http_priority_repeated_headers_combine_members_and_replace_invalid_values) {
    const std::array headers{ruvia::HttpHeaderView{"Priority", "u=2, i"},
        ruvia::HttpHeaderView{"X-Other", "u=0"},
        ruvia::HttpHeaderView{"pRIORITY", "u=\"ignored\""}};
    const auto parsed = ruvia::parseHttpPriority(headers);
    RUVIA_CHECK(parsed && !parsed->urgency && parsed->incremental == true);
    const std::array malformed{ruvia::HttpHeaderView{"priority", "u=2"},
        ruvia::HttpHeaderView{"priority", "i=?2"}};
    RUVIA_CHECK(!ruvia::parseHttpPriority(malformed));
    const std::array emptyRepeated{ruvia::HttpHeaderView{"priority", ""},
        ruvia::HttpHeaderView{"priority", "u=1"}};
    RUVIA_CHECK(!ruvia::parseHttpPriority(emptyRepeated));
    RUVIA_CHECK(ruvia::parseHttpPriority(std::span<const ruvia::HttpHeaderView>{})->requestPriority().urgency == 3);
}

RUVIA_TEST(http_priority_http2_http3_frame_roundtrips_and_output_bounds) {
    std::array<char, 64> bytes{};
    auto h2 = ruvia::encodeHttp2PriorityUpdate(bytes, 5, {.urgency = 1, .incremental = true});
    RUVIA_CHECK(h2.has_value());
    auto decoded = ruvia::decodeHttp2PriorityUpdate(std::span(bytes).subspan(9, *h2 - 9));
    RUVIA_CHECK(decoded && decoded->elementId == 5 && !decoded->push && decoded->fields.urgency == 1);
    for (const bool push : {false, true}) {
        const auto h3 = ruvia::encodeHttp3PriorityUpdate(bytes, {.elementId = push ? 9u : 12u, .push = push, .fields = {.incremental = false}});
        RUVIA_CHECK(h3.has_value());
        const auto frame = ruvia::decodeHttp3FrameHeader(std::span(bytes).first(*h3));
        RUVIA_CHECK(frame.has_value());
        auto value = ruvia::decodeHttp3PriorityUpdate(frame->type, std::span(bytes).subspan(frame->encodedBytes, *h3 - frame->encodedBytes));
        RUVIA_CHECK(value && value->push == push && value->fields.incremental == false);
    }
    RUVIA_CHECK(!ruvia::encodeHttpPriority(std::span(bytes).first(1), {.urgency = 2}));
    RUVIA_CHECK(!ruvia::encodeHttp2PriorityUpdate(bytes, 0, {}));
}
