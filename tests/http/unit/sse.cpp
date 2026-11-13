#include "ruvia/http/sse.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::pmr::string render(const ruvia::sse_message& message) {
    const auto frame = ruvia::format_sse_message(message);
    return std::pmr::string(frame.data(), frame.size());
}

struct parsed_event final {
    std::string type_;
    std::string data_;
    std::string id_;

    bool operator==(const parsed_event&) const = default;
};

// Feeds every chunk, re-offering each unconsumed suffix, until the parser
// leaves the active state.
std::vector<parsed_event> parse_chunks(ruvia::sse_parser& parser, std::initializer_list<std::string_view> chunks) {
    std::vector<parsed_event> events;
    for (auto chunk : chunks) {
        while (!chunk.empty() && parser.state() == ruvia::sse_parser_state::active) {
            const auto result = parser.parse(chunk);
            EXPECT_LE(result.consumed_bytes_, chunk.size());
            chunk.remove_prefix(result.consumed_bytes_);
            if (result.event_) {
                events.push_back({std::string(result.event_->type()), std::string(result.event_->data()),
                    std::string(result.event_->last_event_id())});
            } else if (parser.state() == ruvia::sse_parser_state::active) {
                EXPECT_TRUE(chunk.empty());
            }
        }
    }
    return events;
}

std::vector<parsed_event> parse_stream(std::initializer_list<std::string_view> chunks) {
    ruvia::sse_parser parser;
    return parse_chunks(parser, chunks);
}

constexpr std::string_view whatwg_stream =
    ": comment\n"
    "event: update\n"
    "id: 7\n"
    "data: first\n"
    "data:second\n"
    "data\n"
    "unknown: x\n"
    "\n"
    "data: next\n"
    "\n"
    "id\n"
    "data:  spaced\n"
    "\n";

const std::vector<parsed_event> whatwg_events{
    {"update", "first\nsecond\n", "7"},
    {"message", "next", "7"},
    {"message", " spaced", ""},
};

}  // namespace

TEST(Ruvia, sse_formats_event_id_retry_and_multiline_data) {
    EXPECT_EQ(render({.data_ = "line1\nline2",
                  .event_ = "update",
                  .id_ = "7",
                  .retry_ = std::chrono::milliseconds{3000}}),
        std::pmr::string("event: update\nid: 7\nretry: 3000\ndata: line1\ndata: line2\n\n"));
}

TEST(Ruvia, sse_distinguishes_absent_and_empty_data) {
    EXPECT_EQ(render({.retry_ = std::chrono::milliseconds{3000}}), std::pmr::string("retry: 3000\n\n"));
    EXPECT_EQ(render({.event_ = "ping"}), std::pmr::string("event: ping\n\n"));
    EXPECT_EQ(render({}), std::pmr::string("\n"));
    EXPECT_EQ(render({.data_ = ""}), std::pmr::string("data: \n\n"));
    EXPECT_EQ(render({.id_ = std::string_view{}}), std::pmr::string("id:\n\n"));
    EXPECT_EQ(render({.data_ = "hi"}), std::pmr::string("data: hi\n\n"));
    EXPECT_TRUE(render({.retry_ = std::chrono::milliseconds{1}}).find("data:") == std::pmr::string::npos);
}

TEST(Ruvia, sse_splits_data_on_cr_crlf_and_lf_never_emitting_raw_cr) {
    EXPECT_EQ(render({.data_ = "a\rb"}), std::pmr::string("data: a\ndata: b\n\n"));
    EXPECT_EQ(render({.data_ = "a\nb"}), std::pmr::string("data: a\ndata: b\n\n"));
    EXPECT_EQ(render({.data_ = "a\r\nb"}), std::pmr::string("data: a\ndata: b\n\n"));
    EXPECT_EQ(render({.data_ = "a\r\rb"}), std::pmr::string("data: a\ndata: \ndata: b\n\n"));
    EXPECT_TRUE(!(render({.data_ = "x\ry\r\rz"}).find('\r') != std::string_view::npos));
}

TEST(Ruvia, sse_rejects_newline_in_event_or_id_and_nul_in_id) {
    const auto throws_for = [](ruvia::sse_message message) {
        try {
            (void)ruvia::format_sse_message(message);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    EXPECT_TRUE(throws_for({.data_ = "x", .event_ = "a\nb"}));
    EXPECT_TRUE(throws_for({.data_ = "x", .event_ = "a\rb"}));
    EXPECT_TRUE(throws_for({.id_ = "1\n2"}));
    EXPECT_TRUE(throws_for({.id_ = std::string_view{"a\0b", 3}}));
    EXPECT_TRUE(throws_for({.retry_ = std::chrono::milliseconds{-1}}));

    // Every textual field is UTF-8; a stray byte or a surrogate never reaches the stream.
    EXPECT_TRUE(throws_for({.data_ = std::string_view("\xff", 1)}));
    EXPECT_TRUE(throws_for({.event_ = std::string_view("\xc3", 1)}));
    EXPECT_TRUE(throws_for({.id_ = std::string_view("\xed\xa0\x80", 3)}));
    EXPECT_EQ(render({.data_ = "caf\xc3\xa9", .event_ = "\xe2\x82\xac", .id_ = "\xf0\x9f\x98\x80"}), std::pmr::string("event: \xe2\x82\xac\nid: \xf0\x9f\x98\x80\ndata: caf\xc3\xa9\n\n"));
}

// WHATWG HTML "Interpreting an event stream": one optional space after the
// colon, a colon-less line names a field with an empty value, the event type
// resets after each dispatch while the last event ID persists, and comments and
// unknown fields are ignored.
TEST(Ruvia, sse_parser_interprets_fields_per_whatwg) {
    ruvia::sse_parser parser;
    EXPECT_EQ(parse_chunks(parser, {whatwg_stream}), whatwg_events);
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::active);
    EXPECT_EQ(parser.last_event_id(), "");

    // A block without data dispatches nothing but still commits its id.
    EXPECT_TRUE(parse_chunks(parser, {"id: 9\nevent: ignored\n\n"}).empty());
    EXPECT_EQ(parser.last_event_id(), "9");
    EXPECT_EQ(parse_chunks(parser, {"data:\n\n"}), (std::vector<parsed_event>{{"message", "", "9"}}));
}

TEST(Ruvia, sse_parser_is_independent_of_chunk_boundaries) {
    for (std::size_t split = 0; split <= whatwg_stream.size(); ++split) {
        EXPECT_EQ(parse_stream({whatwg_stream.substr(0, split), whatwg_stream.substr(split)}), whatwg_events)
            << "split at " << split;
    }
    ruvia::sse_parser parser;
    std::vector<parsed_event> events;
    for (std::size_t index = 0; index < whatwg_stream.size(); ++index) {
        const auto chunk_events = parse_chunks(parser, {whatwg_stream.substr(index, 1)});
        events.insert(events.end(), chunk_events.begin(), chunk_events.end());
    }
    EXPECT_EQ(events, whatwg_events);
}

TEST(Ruvia, sse_parser_returns_one_event_per_call_with_consumed_bytes) {
    ruvia::sse_parser parser;
    constexpr std::string_view input = "data: a\n\ndata: b\n\n: tail";
    const auto first = parser.parse(input);
    ASSERT_TRUE(first.event_.has_value());
    EXPECT_EQ(first.consumed_bytes_, std::size_t{9});
    EXPECT_EQ(first.event_->data(), "a");
    const auto second = parser.parse(input.substr(first.consumed_bytes_));
    ASSERT_TRUE(second.event_.has_value());
    EXPECT_EQ(second.consumed_bytes_, std::size_t{9});
    EXPECT_EQ(second.event_->data(), "b");
    const auto rest = parser.parse(input.substr(18));
    EXPECT_FALSE(rest.event_.has_value());
    EXPECT_EQ(rest.consumed_bytes_, std::size_t{6});
}

TEST(Ruvia, sse_parser_ends_lines_at_crlf_lf_and_cr_across_chunks) {
    EXPECT_EQ(parse_stream({"data: a\r\ndata: b\rdata: c\n\r\n"}),
        (std::vector<parsed_event>{{"message", "a\nb\nc", ""}}));
    // An LF opening the next chunk completes the CRLF the previous one began;
    // it is not a blank line that would dispatch early.
    EXPECT_EQ(parse_stream({"data: a\r", "\ndata: b\r", "\n\r", "\n"}),
        (std::vector<parsed_event>{{"message", "a\nb", ""}}));
    EXPECT_EQ(parse_stream({"data: a\r", "data: b\r\r"}), (std::vector<parsed_event>{{"message", "a\nb", ""}}));
    EXPECT_EQ(parse_stream({"data: a\r", "\r"}), (std::vector<parsed_event>{{"message", "a", ""}}));
}

TEST(Ruvia, sse_parser_skips_one_leading_bom) {
    EXPECT_EQ(parse_stream({"\xEF", "\xBB", "\xBF", "data: x\n\n"}), (std::vector<parsed_event>{{"message", "x", ""}}));
    // Only one BOM is stripped; a second one starts the first field name.
    EXPECT_EQ(parse_stream({"\xEF\xBB\xBF\xEF\xBB\xBF"
                            "data: y\n\ndata: z\n\n"}),
        (std::vector<parsed_event>{{"message", "z", ""}}));
    // A partial BOM is ordinary input.
    EXPECT_EQ(parse_stream({"\xEF\xBB", "data: y\n\ndata: z\n\n"}), (std::vector<parsed_event>{{"message", "z", ""}}));
    EXPECT_EQ(parse_stream({"\xEF", "\n\ndata: z\n\n"}), (std::vector<parsed_event>{{"message", "z", ""}}));
}

TEST(Ruvia, sse_parser_accepts_only_ascii_digit_retry_values) {
    ruvia::sse_parser parser;
    EXPECT_EQ(parser.reconnection_time(), std::nullopt);
    EXPECT_TRUE(parse_chunks(parser, {"retry: 3000\n"}).empty());
    EXPECT_EQ(parser.reconnection_time(), std::chrono::milliseconds{3000});
    for (const std::string_view invalid :
        {"retry: 12a\n", "retry:\n", "retry\n", "retry: -5\n", "retry:  7\n", "retry: 1.5\n", "retry: +1\n"}) {
        EXPECT_TRUE(parse_chunks(parser, {invalid}).empty());
        EXPECT_EQ(parser.reconnection_time(), std::chrono::milliseconds{3000}) << invalid;
    }
    EXPECT_TRUE(parse_chunks(parser, {"retry: 12", "34\r\n"}).empty());
    EXPECT_EQ(parser.reconnection_time(), std::chrono::milliseconds{1234});
    EXPECT_TRUE(parse_chunks(parser, {"retry: 0\n"}).empty());
    EXPECT_EQ(parser.reconnection_time(), std::chrono::milliseconds{0});
    EXPECT_TRUE(parse_chunks(parser, {"retry: 99999999999999999999999999\n"}).empty());
    EXPECT_EQ(parser.reconnection_time(), std::chrono::milliseconds::max());
}

TEST(Ruvia, sse_parser_replaces_ill_formed_utf8_and_ignores_ids_with_nul) {
    constexpr std::string_view input("id: a\0b\nevent: \xE2\x82\ndata: \xFF\xC3\ndata: caf\xC3\xA9\n\n", 40);
    EXPECT_EQ(parse_stream({input}),
        (std::vector<parsed_event>{{"\xEF\xBF\xBD", "\xEF\xBF\xBD\xEF\xBF\xBD\ncaf\xC3\xA9", ""}}));
    // A truncated sequence at a line end is replaced; the LF stays a separator.
    EXPECT_EQ(parse_stream({"data: \xE2\x82\ndata: \xAC\n\n"}),
        (std::vector<parsed_event>{{"message", "\xEF\xBF\xBD\n\xEF\xBF\xBD", ""}}));
}

TEST(Ruvia, sse_parser_fails_an_event_over_its_limit_and_reset_reuses_it) {
    ruvia::sse_parser parser({.event_limit_ = ruvia::protocol_byte_limit::limited(8)});
    // Seven data bytes and their LF fill the limit exactly.
    EXPECT_EQ(parse_chunks(parser, {"id: 1\ndata: 1234567\n\n"}), (std::vector<parsed_event>{{"message", "1234567", "1"}}));
    // Comments and ignored fields buffer nothing, whatever their length.
    EXPECT_TRUE(parse_chunks(parser, {": " + std::string(1000, 'c') + "\nignored: " + std::string(1000, 'i') + "\n"}).empty());
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::active);

    const auto failed = parser.parse("data: 1234\ndata: 567\n\n");
    EXPECT_FALSE(failed.event_.has_value());
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::failed);
    EXPECT_THROW((void)parser.parse("data: x\n\n"), std::logic_error);
    parser.finish();
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::failed);

    parser.reset();
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::active);
    EXPECT_EQ(parser.last_event_id(), "");
    EXPECT_EQ(parse_chunks(parser, {"\xEF\xBB\xBF"
                                    "data: ok\n\n"}),
        (std::vector<parsed_event>{{"message", "ok", ""}}));

    // A pending event type counts too.
    EXPECT_TRUE(parse_chunks(parser, {"event: 123456789\n"}).empty());
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::failed);
}

TEST(Ruvia, sse_parser_finish_discards_an_incomplete_event) {
    ruvia::sse_parser parser;
    EXPECT_EQ(parse_chunks(parser, {"id: 9\n\nretry: 50\ndata: pending\n", "data: partial"}), std::vector<parsed_event>{});
    parser.finish();
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::finished);
    EXPECT_EQ(parser.last_event_id(), "9");
    EXPECT_EQ(parser.reconnection_time(), std::chrono::milliseconds{50});
    EXPECT_THROW((void)parser.parse("\n"), std::logic_error);

    parser.reset();
    EXPECT_EQ(parser.reconnection_time(), std::nullopt);
    EXPECT_EQ(parse_chunks(parser, {"data: fresh\n\n"}), (std::vector<parsed_event>{{"message", "fresh", ""}}));
}
