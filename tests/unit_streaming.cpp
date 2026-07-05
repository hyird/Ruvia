#include "test_harness.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <chrono>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "http/StreamingInternal.h"
#include "net/server/HttpResponseStreamState.h"
#include "runtime/AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Streaming.h"

namespace {

struct CaptureStreamSink final {
    std::pmr::string scratch{std::pmr::get_default_resource()};
    std::vector<std::string> writes;
};

ruvia::Task<void> writeChunk(void* target, std::string_view chunk) {
    static_cast<CaptureStreamSink*>(target)->writes.emplace_back(chunk);
    co_return;
}

ruvia::Task<void> endStream(void*) {
    co_return;
}

ruvia::Task<void> sleepStream(void*, std::chrono::milliseconds) {
    co_return;
}

void bindContext(void*, ruvia::Context*) noexcept {}

std::pmr::string& scratch(void* target) noexcept {
    return static_cast<CaptureStreamSink*>(target)->scratch;
}

void addTrailer(void*, std::string_view, std::string_view) {}

bool committed(void*) noexcept {
    return false;
}

bool aborted(void*) noexcept {
    return false;
}

ruvia::ResponseStreamWriter makeWriter(CaptureStreamSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(
        &sink,
        &writeChunk,
        &endStream,
        &sleepStream,
        &bindContext,
        &scratch,
        &addTrailer,
        &committed,
        &aborted);
}

ruvia::Task<void> writeLines(ruvia::ResponseStreamWriter& writer) {
    co_await writer.writeln("first");
    co_await writer.writeln("second");
}

}  // namespace

RUVIA_TEST(response_stream_writeln_reuses_scratch_without_leaking_previous_chunk) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(writeLines(writer)),
        asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{2});
    RUVIA_CHECK_EQ(sink.writes[0], std::string("first\n"));
    RUVIA_CHECK_EQ(sink.writes[1], std::string("second\n"));
}

namespace {

ruvia::Task<void> writeOneSse(ruvia::SseWriter& sse, ruvia::SseMessage message) {
    co_await sse.writeSSE(message);
}

}  // namespace

RUVIA_TEST(sse_writer_formats_event_id_retry_and_multiline_data) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);
    auto sse = ruvia::detail::StreamingAccess::makeSseWriter(writer);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(writeOneSse(
            sse, ruvia::SseMessage{.data = "line1\nline2", .event = "update", .id = "7",
                                   .retry = 3000})),
        asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{1});
    // Header fields, one "data:" line per input line, then the blank line that ends
    // the event. An embedded newline in data is split, never emitted raw.
    RUVIA_CHECK_EQ(
        sink.writes[0],
        std::string("event: update\nid: 7\nretry: 3000\ndata: line1\ndata: line2\n\n"));
}

RUVIA_TEST(sse_writer_splits_data_on_cr_crlf_and_lf_never_emitting_raw_cr) {
    // EventSource splits lines on CR, LF, or CRLF, so a bare CR in data must be
    // treated as a line break too -- otherwise it would survive into a "data:"
    // line and the client would reinterpret it, injecting fields or (via a blank
    // line from "\r\r") a whole new event.
    const auto render = [](std::string_view data) {
        CaptureStreamSink sink;
        auto writer = makeWriter(sink);
        auto sse = ruvia::detail::StreamingAccess::makeSseWriter(writer);
        asio::io_context ctx(1);
        auto future = asio::co_spawn(
            ctx,
            ruvia::detail::taskAsAwaitable(writeOneSse(sse, ruvia::SseMessage{.data = data})),
            asio::use_future);
        ctx.run();
        future.get();
        return sink.writes.empty() ? std::string{} : sink.writes[0];
    };

    // Bare CR, LF, and CRLF all split into separate data lines; identical output.
    RUVIA_CHECK_EQ(render("a\rb"), std::string("data: a\ndata: b\n\n"));
    RUVIA_CHECK_EQ(render("a\nb"), std::string("data: a\ndata: b\n\n"));
    RUVIA_CHECK_EQ(render("a\r\nb"), std::string("data: a\ndata: b\n\n"));
    // "\r\r" would be a blank line (event dispatch) to the client -> must become
    // two splits (an empty data line between), with no raw CR anywhere.
    RUVIA_CHECK_EQ(render("a\r\rb"), std::string("data: a\ndata: \ndata: b\n\n"));
    // No raw CR byte may appear in any rendered frame.
    RUVIA_CHECK(render("x\ry\r\rz").find('\r') == std::string::npos);
}

RUVIA_TEST(sse_writer_rejects_newline_in_event_or_id) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);
    auto sse = ruvia::detail::StreamingAccess::makeSseWriter(writer);

    const auto throwsFor = [&](ruvia::SseMessage message) {
        asio::io_context ctx(1);
        auto future = asio::co_spawn(
            ctx, ruvia::detail::taskAsAwaitable(writeOneSse(sse, message)), asio::use_future);
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
    RUVIA_CHECK(throwsFor(ruvia::SseMessage{.data = "x", .event = "a\nb"}));
    RUVIA_CHECK(throwsFor(ruvia::SseMessage{.data = "x", .event = "a\rb"}));
    RUVIA_CHECK(throwsFor(ruvia::SseMessage{.id = "1\n2"}));
    // A rejected message emits nothing.
    RUVIA_CHECK(sink.writes.empty());
}

RUVIA_TEST(response_stream_state_rejects_body_and_trailers_after_end) {
    using ruvia::detail::ResponseStreamState;
    // A committed stream that allows a body accepts a chunk before end()...
    ResponseStreamState open;
    open.markCommitted(false);
    open.ensureBodyAllowed();  // no throw

    // ...but after end() a further body chunk would land past the terminal
    // 0\r\n\r\n (HTTP/1.1) or END_STREAM (HTTP/2) and desync the connection, so
    // it must be rejected -- the same way a post-end trailer already is.
    open.markEnded();
    bool bodyAfterEnd = false;
    try {
        open.ensureBodyAllowed();
    } catch (const std::logic_error&) {
        bodyAfterEnd = true;
    }
    RUVIA_CHECK(bodyAfterEnd);
    bool trailerAfterEnd = false;
    try {
        open.ensureTrailerAllowed("X-Trailer", "v");
    } catch (const std::logic_error&) {
        trailerAfterEnd = true;
    }
    RUVIA_CHECK(trailerAfterEnd);

    // A body-forbidden status (e.g. 204/304) still rejects a body chunk.
    ResponseStreamState forbidden;
    forbidden.markCommitted(true);
    bool bodyForbidden = false;
    try {
        forbidden.ensureBodyAllowed();
    } catch (const std::logic_error&) {
        bodyForbidden = true;
    }
    RUVIA_CHECK(bodyForbidden);
}
