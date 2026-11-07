#include "ruvia/http/sse.h"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string render(const ruvia::sse_message& message) {
    const auto frame = ruvia::format_sse_message(message);
    return std::string(frame.data(), frame.size());
}

}  // namespace

TEST(Ruvia, sse_formats_event_id_retry_and_multiline_data) {
    EXPECT_EQ(render({.data_ = "line1\nline2",
                  .event_ = "update",
                  .id_ = "7",
                  .retry_ = std::chrono::milliseconds{3000}}),
        std::string("event: update\nid: 7\nretry: 3000\ndata: line1\ndata: line2\n\n"));
}

TEST(Ruvia, sse_distinguishes_absent_and_empty_data) {
    EXPECT_EQ(render({.retry_ = std::chrono::milliseconds{3000}}), std::string("retry: 3000\n\n"));
    EXPECT_EQ(render({.event_ = "ping"}), std::string("event: ping\n\n"));
    EXPECT_EQ(render({}), std::string("\n"));
    EXPECT_EQ(render({.data_ = ""}), std::string("data: \n\n"));
    EXPECT_EQ(render({.id_ = std::string_view{}}), std::string("id:\n\n"));
    EXPECT_EQ(render({.data_ = "hi"}), std::string("data: hi\n\n"));
    EXPECT_TRUE(render({.retry_ = std::chrono::milliseconds{1}}).find("data:") == std::string::npos);
}

TEST(Ruvia, sse_splits_data_on_cr_crlf_and_lf_never_emitting_raw_cr) {
    EXPECT_EQ(render({.data_ = "a\rb"}), std::string("data: a\ndata: b\n\n"));
    EXPECT_EQ(render({.data_ = "a\nb"}), std::string("data: a\ndata: b\n\n"));
    EXPECT_EQ(render({.data_ = "a\r\nb"}), std::string("data: a\ndata: b\n\n"));
    EXPECT_EQ(render({.data_ = "a\r\rb"}), std::string("data: a\ndata: \ndata: b\n\n"));
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
    EXPECT_EQ(render({.data_ = "caf\xc3\xa9", .event_ = "\xe2\x82\xac", .id_ = "\xf0\x9f\x98\x80"}), std::string("event: \xe2\x82\xac\nid: \xf0\x9f\x98\x80\ndata: caf\xc3\xa9\n\n"));
}
