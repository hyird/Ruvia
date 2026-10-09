#include "ruvia/http/sse.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

#include "test_harness.h"

namespace {

std::string render(const ruvia::sse_message& message) {
    const auto frame = ruvia::format_sse_message(message);
    return std::string(frame.data(), frame.size());
}

}  // namespace

RUVIA_TEST(sse_formats_event_id_retry_and_multiline_data) {
    RUVIA_CHECK_EQ(render({.data_ = "line1\nline2",
                       .event_ = "update",
                       .id_ = "7",
                       .retry_ = std::chrono::milliseconds{3000}}),
        std::string("event: update\nid: 7\nretry: 3000\ndata: line1\ndata: line2\n\n"));
}

RUVIA_TEST(sse_distinguishes_absent_and_empty_data) {
    RUVIA_CHECK_EQ(render({.retry_ = std::chrono::milliseconds{3000}}), std::string("retry: 3000\n\n"));
    RUVIA_CHECK_EQ(render({.event_ = "ping"}), std::string("event: ping\n\n"));
    RUVIA_CHECK_EQ(render({}), std::string("\n"));
    RUVIA_CHECK_EQ(render({.data_ = ""}), std::string("data: \n\n"));
    RUVIA_CHECK_EQ(render({.id_ = std::string_view{}}), std::string("id:\n\n"));
    RUVIA_CHECK_EQ(render({.data_ = "hi"}), std::string("data: hi\n\n"));
    RUVIA_CHECK(render({.retry_ = std::chrono::milliseconds{1}}).find("data:") == std::string::npos);
}

RUVIA_TEST(sse_splits_data_on_cr_crlf_and_lf_never_emitting_raw_cr) {
    RUVIA_CHECK_EQ(render({.data_ = "a\rb"}), std::string("data: a\ndata: b\n\n"));
    RUVIA_CHECK_EQ(render({.data_ = "a\nb"}), std::string("data: a\ndata: b\n\n"));
    RUVIA_CHECK_EQ(render({.data_ = "a\r\nb"}), std::string("data: a\ndata: b\n\n"));
    RUVIA_CHECK_EQ(render({.data_ = "a\r\rb"}), std::string("data: a\ndata: \ndata: b\n\n"));
    RUVIA_CHECK(!(render({.data_ = "x\ry\r\rz"}).find('\r') != std::string_view::npos));
}

RUVIA_TEST(sse_rejects_newline_in_event_or_id_and_nul_in_id) {
    const auto throws_for = [](ruvia::sse_message message) {
        try {
            (void)ruvia::format_sse_message(message);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    RUVIA_CHECK(throws_for({.data_ = "x", .event_ = "a\nb"}));
    RUVIA_CHECK(throws_for({.data_ = "x", .event_ = "a\rb"}));
    RUVIA_CHECK(throws_for({.id_ = "1\n2"}));
    RUVIA_CHECK(throws_for({.id_ = std::string_view{"a\0b", 3}}));
    RUVIA_CHECK(throws_for({.retry_ = std::chrono::milliseconds{-1}}));
}
