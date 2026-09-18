#include "ruvia/http/Sse.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

#include "test_harness.h"

namespace {

std::string render(const ruvia::SseMessage& message) {
    const auto frame = ruvia::formatSseMessage(message);
    return std::string(frame.data(), frame.size());
}

}  // namespace

RUVIA_TEST(sse_formats_event_id_retry_and_multiline_data) {
    RUVIA_CHECK_EQ(render({.data = "line1\nline2",
                       .event = "update",
                       .id = "7",
                       .retry = std::chrono::milliseconds{3000}}),
        std::string("event: update\nid: 7\nretry: 3000\ndata: line1\ndata: line2\n\n"));
}

RUVIA_TEST(sse_distinguishes_absent_and_empty_data) {
    RUVIA_CHECK_EQ(render({.retry = std::chrono::milliseconds{3000}}), std::string("retry: 3000\n\n"));
    RUVIA_CHECK_EQ(render({.event = "ping"}), std::string("event: ping\n\n"));
    RUVIA_CHECK_EQ(render({}), std::string("\n"));
    RUVIA_CHECK_EQ(render({.data = ""}), std::string("data: \n\n"));
    RUVIA_CHECK_EQ(render({.id = std::string_view{}}), std::string("id:\n\n"));
    RUVIA_CHECK_EQ(render({.data = "hi"}), std::string("data: hi\n\n"));
    RUVIA_CHECK(render({.retry = std::chrono::milliseconds{1}}).find("data:") == std::string::npos);
}

RUVIA_TEST(sse_splits_data_on_cr_crlf_and_lf_never_emitting_raw_cr) {
    RUVIA_CHECK_EQ(render({.data = "a\rb"}), std::string("data: a\ndata: b\n\n"));
    RUVIA_CHECK_EQ(render({.data = "a\nb"}), std::string("data: a\ndata: b\n\n"));
    RUVIA_CHECK_EQ(render({.data = "a\r\nb"}), std::string("data: a\ndata: b\n\n"));
    RUVIA_CHECK_EQ(render({.data = "a\r\rb"}), std::string("data: a\ndata: \ndata: b\n\n"));
    RUVIA_CHECK(!render({.data = "x\ry\r\rz"}).contains('\r'));
}

RUVIA_TEST(sse_rejects_newline_in_event_or_id_and_nul_in_id) {
    const auto throwsFor = [](ruvia::SseMessage message) {
        try {
            (void)ruvia::formatSseMessage(message);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    RUVIA_CHECK(throwsFor({.data = "x", .event = "a\nb"}));
    RUVIA_CHECK(throwsFor({.data = "x", .event = "a\rb"}));
    RUVIA_CHECK(throwsFor({.id = "1\n2"}));
    RUVIA_CHECK(throwsFor({.id = std::string_view{"a\0b", 3}}));
    RUVIA_CHECK(throwsFor({.retry = std::chrono::milliseconds{-1}}));
}
