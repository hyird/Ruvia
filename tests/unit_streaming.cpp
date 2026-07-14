#include "test_harness.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/detail/http/StreamingInternal.h"
#include "ruvia/web/detail/server/HttpResponseStreamState.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/Streaming.h"

namespace {

struct CaptureStreamSink final {
    std::pmr::string scratch{std::pmr::get_default_resource()};
    std::vector<std::string> writes;
    std::vector<std::string> trailers;
};

ruvia::Task<void> writeChunk(void* target, std::string_view chunk) {
    static_cast<CaptureStreamSink*>(target)->writes.emplace_back(chunk);
    co_return;
}

ruvia::Task<void> endStream(
    void* target,
    std::span<const ruvia::HttpHeaderView> trailers) {
    auto& captured = static_cast<CaptureStreamSink*>(target)->trailers;
    for (const auto& trailer : trailers) {
        captured.emplace_back(
            std::string(trailer.name()) + "=" + std::string(trailer.value()));
    }
    co_return;
}

ruvia::Task<void> sleepStream(void*, std::chrono::milliseconds) {
    co_return;
}

void bindContext(void*, ruvia::Context*, ruvia::HttpResponse (*)(ruvia::Context&)) noexcept {}

std::pmr::string& scratch(void* target) noexcept {
    return static_cast<CaptureStreamSink*>(target)->scratch;
}

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
        &committed,
        &aborted);
}

ruvia::Task<void> writeLines(ruvia::ResponseStreamWriter& writer) {
    co_await writer.writeln("first");
    co_await writer.writeln("second");
}

ruvia::Task<void> endWithTrailers(ruvia::ResponseStreamWriter& writer) {
    const std::array<ruvia::HttpHeaderView, 2> trailers{
        ruvia::HttpHeaderView{"Digest", "sha-256=value"},
        ruvia::HttpHeaderView{"Server-Timing", "db;dur=7"}};
    co_await writer.end(trailers);
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

RUVIA_TEST(response_stream_end_submits_one_terminal_trailer_section) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(endWithTrailers(writer)),
        asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.trailers.size(), std::size_t{2});
    RUVIA_CHECK_EQ(sink.trailers[0], std::string("Digest=sha-256=value"));
    RUVIA_CHECK_EQ(sink.trailers[1], std::string("Server-Timing=db;dur=7"));
}

namespace {

ruvia::Task<void> writeOneSse(ruvia::SseWriter& sse, ruvia::SseMessage message) {
    co_await sse.write(message);
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

RUVIA_TEST(sse_writer_omits_data_line_for_empty_data_no_phantom_event) {
    // WHATWG HTML 9.2.6: a block whose data buffer is empty must NOT dispatch. An
    // unconditional "data:" line makes the client's data buffer "\n" (non-empty),
    // so it would strip the trailing LF and fire a phantom empty message event.
    // An empty-data block must therefore emit no data field at all.
    const auto render = [](ruvia::SseMessage message) {
        CaptureStreamSink sink;
        auto writer = makeWriter(sink);
        auto sse = ruvia::detail::StreamingAccess::makeSseWriter(writer);
        asio::io_context ctx(1);
        auto future = asio::co_spawn(
            ctx, ruvia::detail::taskAsAwaitable(writeOneSse(sse, message)), asio::use_future);
        ctx.run();
        future.get();
        return sink.writes.empty() ? std::string{} : sink.writes[0];
    };

    // A retry-only block bumps the reconnection time and dispatches nothing.
    RUVIA_CHECK_EQ(render(ruvia::SseMessage{.retry = 3000}), std::string("retry: 3000\n\n"));
    // An event-only block likewise emits no data field (empty data never dispatches).
    RUVIA_CHECK_EQ(render(ruvia::SseMessage{.event = "ping"}), std::string("event: ping\n\n"));
    // A bare block is a no-op keepalive: just the terminating blank line.
    RUVIA_CHECK_EQ(render(ruvia::SseMessage{}), std::string("\n"));
    // Data present is unaffected: data lines are still emitted.
    RUVIA_CHECK_EQ(render(ruvia::SseMessage{.data = "hi"}), std::string("data: hi\n\n"));
    // No empty-data frame ever carries a "data:" line.
    RUVIA_CHECK(render(ruvia::SseMessage{.retry = 1}).find("data:") == std::string::npos);
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

RUVIA_TEST(sse_writer_rejects_nul_in_id) {
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

    // A U+0000 NUL in the id makes a compliant EventSource client ignore the id
    // (WHATWG HTML 9.2.6), silently breaking Last-Event-ID resumption, so it is
    // rejected -- mirroring the CR/LF guard. The NUL sits mid-value to prove the
    // whole field is scanned, not just a prefix.
    RUVIA_CHECK(throwsFor(ruvia::SseMessage{.id = std::string_view("a\0b", 3)}));
    // event and data carry no such rule, so a NUL there is accepted and emitted.
    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(
            writeOneSse(sse, ruvia::SseMessage{.data = std::string_view("d\0e", 3),
                                               .event = std::string_view("v\0w", 3)})),
        asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(!sink.writes.empty());
    // The rejected-id message emitted nothing; only the accepted message wrote.
    RUVIA_CHECK_EQ(sink.writes.size(), static_cast<std::size_t>(1));
}

RUVIA_TEST(response_stream_state_drives_typed_post_head_phases) {
    using ruvia::detail::ResponseStreamState;
    // A committed stream that allows a body accepts a chunk before end()...
    ResponseStreamState open;
    open.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(
        ruvia::detail::ResponseStreamFraming::kHttp1Chunked,
        ruvia::HttpKnownMethod::kGet,
        207,
        ruvia::detail::ResponseTrailerIntent::kNone));
    RUVIA_CHECK(open.commitPlan() != nullptr);
    RUVIA_CHECK_EQ(
        open.commitPlan()->responseStatus(),
        std::uint16_t{207});
    RUVIA_CHECK(
        open.commitPlan()->framing() ==
        ruvia::detail::ResponseStreamFraming::kHttp1Chunked);
    bool recommitRejected = false;
    try {
        open.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(
            ruvia::detail::ResponseStreamFraming::kHttp2Frames,
            ruvia::HttpKnownMethod::kGet,
            418,
            ruvia::detail::ResponseTrailerIntent::kNone));
    } catch (const std::logic_error&) {
        recommitRejected = true;
    }
    RUVIA_CHECK(recommitRejected);
    RUVIA_CHECK_EQ(
        open.commitPlan()->responseStatus(),
        std::uint16_t{207});
    open.ensureBodyAllowed();  // no throw
    open.ensureTrailersAllowed(
        ruvia::detail::ResponseStreamTrailerFraming::kHttp1Chunked);

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
        open.ensureTrailersAllowed(
            ruvia::detail::ResponseStreamTrailerFraming::kHttp1Chunked);
    } catch (const std::logic_error&) {
        trailerAfterEnd = true;
    }
    RUVIA_CHECK(trailerAfterEnd);

    // A suppressed body (e.g. HEAD, 204 or 304) still rejects a body chunk.
    ResponseStreamState suppressed;
    suppressed.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(
        ruvia::detail::ResponseStreamFraming::kHttp1Chunked,
        ruvia::HttpKnownMethod::kHead,
        200,
        ruvia::detail::ResponseTrailerIntent::kNone));
    bool bodyRejected = false;
    try {
        suppressed.ensureBodyAllowed();
    } catch (const std::logic_error&) {
        bodyRejected = true;
    }
    RUVIA_CHECK(bodyRejected);

    // HTTP/2 can keep the same content-forbidden response open solely for a
    // terminal trailing-HEADERS block, without accidentally enabling DATA.
    ResponseStreamState trailersOnly;
    trailersOnly.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(
        ruvia::detail::ResponseStreamFraming::kHttp2Frames,
        ruvia::HttpKnownMethod::kHead,
        200,
        ruvia::detail::ResponseTrailerIntent::kPresent));
    RUVIA_CHECK(trailersOnly.committed());
    RUVIA_CHECK(!trailersOnly.ended());
    bool trailersOnlyBodyRejected = false;
    try {
        trailersOnly.ensureBodyAllowed();
    } catch (const std::logic_error&) {
        trailersOnlyBodyRejected = true;
    }
    RUVIA_CHECK(trailersOnlyBodyRejected);
    trailersOnly.ensureTrailersAllowed(
        ruvia::detail::ResponseStreamTrailerFraming::kHttp2TrailingHeaders);
}

RUVIA_TEST(response_stream_head_rejects_a_mismatched_status_plan) {
    ruvia::HttpResponse response(std::pmr::get_default_resource());
    response.status(201);
    auto plan = ruvia::detail::httpResponseStreamCommitPlan(
        ruvia::detail::ResponseStreamFraming::kHttp1Chunked,
        ruvia::HttpKnownMethod::kGet,
        202,
        ruvia::detail::ResponseTrailerIntent::kNone);
    bool rejected = false;
    try {
        (void)ruvia::detail::prepareResponseStreamHead(
            std::move(response),
            ruvia::detail::ResponseStreamKind::kGeneric,
            std::move(plan));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
