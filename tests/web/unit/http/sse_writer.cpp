#include <string_view>

#include "ruvia/core/asio_task.h"

#include "streaming_fixture.h"

// Server-sent events: field formatting and the bytes a caller may never inject.

RUVIA_TEST(sse_writer_formats_event_id_retry_and_multiline_data) {
    capture_stream_sink sink;
    auto writer = make_writer(sink);
    auto sse = ruvia::detail::streaming_access::make_sse_writer(writer);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(
            write_one_sse(sse, ruvia::sse_message{.data_ = "line1\nline2",
                                   .event_ = "update",
                                   .id_ = "7",
                                   .retry_ = std::chrono::milliseconds{3000}})),
        asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.writes_.size(), std::size_t{1});
    // Header fields, one "data:" line per input line, then the blank line that ends
    // the event. An embedded newline in data is split, never emitted raw.
    RUVIA_CHECK_EQ(sink.writes_[0],
        std::string("event: update\nid: 7\nretry: 3000\ndata: line1\ndata: line2\n\n"));
}

RUVIA_TEST(sse_writer_distinguishes_absent_and_empty_data) {
    // WHATWG HTML 9.2.6: a block whose data buffer is empty must NOT dispatch,
    // while even an empty `data:` field appends LF and makes that buffer
    // non-empty. The API must preserve that presence distinction.
    const auto render = [](ruvia::sse_message message) {
        capture_stream_sink sink;
        auto writer = make_writer(sink);
        auto sse = ruvia::detail::streaming_access::make_sse_writer(writer);
        asio::io_context ctx(1);
        auto future = asio::co_spawn(
            ctx, ruvia::as_awaitable(write_one_sse(sse, message)), asio::use_future);
        ctx.run();
        future.get();
        return sink.writes_.empty() ? std::string{} : sink.writes_[0];
    };

    // A retry-only block bumps the reconnection time and dispatches nothing.
    RUVIA_CHECK_EQ(render(ruvia::sse_message{.retry_ = std::chrono::milliseconds{3000}}),
        std::string("retry: 3000\n\n"));
    // An event-only block likewise emits no data field (empty data never dispatches).
    RUVIA_CHECK_EQ(render(ruvia::sse_message{.event_ = "ping"}), std::string("event: ping\n\n"));
    // A bare block is a no-op keepalive: just the terminating blank line.
    RUVIA_CHECK_EQ(render(ruvia::sse_message{}), std::string("\n"));
    // A present empty data value is an event: the data field first makes the
    // EventSource data buffer non-empty, then dispatch removes its final LF.
    RUVIA_CHECK_EQ(render(ruvia::sse_message{.data_ = ""}), std::string("data: \n\n"));
    // A present-but-empty id resets the EventSource last-event-ID buffer.
    RUVIA_CHECK_EQ(render(ruvia::sse_message{.id_ = std::string_view{}}), std::string("id:\n\n"));
    // Non-empty data is unaffected: data lines are still emitted.
    RUVIA_CHECK_EQ(render(ruvia::sse_message{.data_ = "hi"}), std::string("data: hi\n\n"));
    // No absent-data frame carries a "data:" line.
    RUVIA_CHECK(render(ruvia::sse_message{.retry_ = std::chrono::milliseconds{1}}).find("data:") ==
                std::string::npos);
}

RUVIA_TEST(sse_writer_splits_data_on_cr_crlf_and_lf_never_emitting_raw_cr) {
    // EventSource splits lines on CR, LF, or CRLF, so a bare CR in data must be
    // treated as a line break too -- otherwise it would survive into a "data:"
    // line and the client would reinterpret it, injecting fields or (via a blank
    // line from "\r\r") a whole new event.
    const auto render = [](std::string_view data) {
        capture_stream_sink sink;
        auto writer = make_writer(sink);
        auto sse = ruvia::detail::streaming_access::make_sse_writer(writer);
        asio::io_context ctx(1);
        auto future = asio::co_spawn(ctx,
            ruvia::as_awaitable(write_one_sse(sse, ruvia::sse_message{.data_ = data})),
            asio::use_future);
        ctx.run();
        future.get();
        return sink.writes_.empty() ? std::string{} : sink.writes_[0];
    };

    // Bare CR, LF, and CRLF all split into separate data lines; identical output.
    RUVIA_CHECK_EQ(render("a\rb"), std::string("data: a\ndata: b\n\n"));
    RUVIA_CHECK_EQ(render("a\nb"), std::string("data: a\ndata: b\n\n"));
    RUVIA_CHECK_EQ(render("a\r\nb"), std::string("data: a\ndata: b\n\n"));
    // "\r\r" would be a blank line (event dispatch) to the client -> must become
    // two splits (an empty data line between), with no raw CR anywhere.
    RUVIA_CHECK_EQ(render("a\r\rb"), std::string("data: a\ndata: \ndata: b\n\n"));
    // No raw CR byte may appear in any rendered frame.
    RUVIA_CHECK(!(render("x\ry\r\rz").find('\r') != std::string_view::npos));
}

RUVIA_TEST(sse_writer_rejects_newline_in_event_or_id) {
    capture_stream_sink sink;
    auto writer = make_writer(sink);
    auto sse = ruvia::detail::streaming_access::make_sse_writer(writer);

    const auto throws_for = [&](ruvia::sse_message message) {
        asio::io_context ctx(1);
        auto future = asio::co_spawn(
            ctx, ruvia::as_awaitable(write_one_sse(sse, message)), asio::use_future);
        ctx.run();
        try {
            future.get();
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };

    // A CR or LF in event/id would inject additional SSE fields or a new event, so
    // it is rejected -- matching how the data field is line-split rather than raw.
    RUVIA_CHECK(throws_for(ruvia::sse_message{.data_ = "x", .event_ = "a\nb"}));
    RUVIA_CHECK(throws_for(ruvia::sse_message{.data_ = "x", .event_ = "a\rb"}));
    RUVIA_CHECK(throws_for(ruvia::sse_message{.id_ = "1\n2"}));
    // A rejected message emits nothing.
    RUVIA_CHECK(sink.writes_.empty());
}

RUVIA_TEST(sse_writer_rejects_nul_in_id) {
    capture_stream_sink sink;
    auto writer = make_writer(sink);
    auto sse = ruvia::detail::streaming_access::make_sse_writer(writer);

    const auto throws_for = [&](ruvia::sse_message message) {
        asio::io_context ctx(1);
        auto future = asio::co_spawn(
            ctx, ruvia::as_awaitable(write_one_sse(sse, message)), asio::use_future);
        ctx.run();
        try {
            future.get();
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };

    // A U+0000 NUL in the id makes a compliant EventSource client ignore the id
    // (WHATWG HTML 9.2.6), silently breaking Last-Event-ID resumption, so it is
    // rejected -- mirroring the CR/LF guard. The NUL sits mid-value to prove the
    // whole field is scanned, not just a prefix.
    RUVIA_CHECK(throws_for(ruvia::sse_message{.id_ = std::string_view("a\0b", 3)}));
    // event and data carry no such rule, so a NUL there is accepted and emitted.
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(
            write_one_sse(sse, ruvia::sse_message{.data_ = std::string_view("d\0e", 3),
                                   .event_ = std::string_view("v\0w", 3)})),
        asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(!sink.writes_.empty());
    // The rejected-id message emitted nothing; only the accepted message wrote.
    RUVIA_CHECK_EQ(sink.writes_.size(), static_cast<std::size_t>(1));
}
