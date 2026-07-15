#include "test_harness.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/detail/http/StreamingInternal.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/web/detail/server/HttpResponseStreamState.h"
#include "ruvia/web/detail/websocket/WebSocketInternal.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/detail/WorkerSignal.h"
#include "ruvia/core/detail/WorkerDispatcher.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/Streaming.h"

namespace {

class TestScopedCapability final : private ruvia::detail::ScopedCapabilityNode {
public:
    TestScopedCapability(
        ruvia::detail::ScopedOperationScope& scope,
        int& expiredCount) noexcept
        : ScopedCapabilityNode(scope, &TestScopedCapability::expire),
          expiredCount_(&expiredCount) {}

    TestScopedCapability(const TestScopedCapability& other) noexcept
        : ScopedCapabilityNode(other),
          expiredCount_(other.expiredCount_) {}

    TestScopedCapability(TestScopedCapability&& other) noexcept
        : ScopedCapabilityNode(std::move(other)),
          expiredCount_(std::exchange(other.expiredCount_, nullptr)) {}

    void use() const { requireActive(); }

private:
    static void expire(ruvia::detail::ScopedCapabilityNode& node) noexcept {
        auto& capability = static_cast<TestScopedCapability&>(node);
        ++*capability.expiredCount_;
    }

    int* expiredCount_;
};

struct ColdFrameProbe final {
    bool* destroyed;
    bool armed{true};

    explicit ColdFrameProbe(bool& value) noexcept : destroyed(&value) {}
    ColdFrameProbe(ColdFrameProbe&& other) noexcept
        : destroyed(other.destroyed), armed(std::exchange(other.armed, false)) {}
    ~ColdFrameProbe() {
        if (armed) {
            *destroyed = true;
        }
    }
};

ruvia::Task<void> coldFrameTask(ColdFrameProbe) { co_return; }

struct CaptureStreamSink final {
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
void releaseContext(void*) noexcept {}

bool committed(void*) noexcept {
    return false;
}

bool aborted(void*) noexcept {
    return false;
}

ruvia::HttpResponse unusedStreamingHead(ruvia::Context&) {
    return ruvia::HttpResponse(std::pmr::get_default_resource());
}

ruvia::ResponseStreamWriter makeWriter(CaptureStreamSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(
        &sink,
        &writeChunk,
        &endStream,
        &sleepStream,
        &bindContext,
        &releaseContext,
        &committed,
        &aborted);
}

ruvia::Task<void> writeLines(ruvia::ResponseStreamWriter& writer) {
    co_await writer.writeln("first");
    co_await writer.writeln("second");
}

ruvia::Task<void> writeStoredLines(ruvia::ResponseStreamWriter& writer) {
    auto first = writer.writeln(std::string("stored-first"));
    auto second = writer.writeln(std::string("stored-second"));
    co_await std::move(first);
    co_await std::move(second);
}

ruvia::ScopedOperation<void> makeExpiredWrite(CaptureStreamSink& sink) {
    auto writer = makeWriter(sink);
    return writer.write(std::string("must-not-run"));
}

ruvia::Task<void> awaitExpiredWrite(
    ruvia::ScopedOperation<void>& operation,
    bool& rejected) {
    try {
        co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

struct CaptureWebSocket final {
    std::vector<std::string> writes;
};

ruvia::Task<std::optional<ruvia::WebSocketMessage>> readSocket(void*) {
    co_return std::nullopt;
}

ruvia::Task<void> writeSocket(
    void* target,
    ruvia::WebSocketOpcode,
    std::string_view payload) {
    static_cast<CaptureWebSocket*>(target)->writes.emplace_back(payload);
    co_return;
}

ruvia::Task<void> closeSocket(void*, std::uint16_t, std::string_view) {
    co_return;
}

ruvia::ScopedOperation<void> makeExpiredWebSocketWrite(
    CaptureWebSocket& capture) {
    auto socket = ruvia::detail::WebSocketAccess::make(
        &capture, &readSocket, &writeSocket, &closeSocket);
    return socket.text(std::string("expired-payload"));
}

ruvia::Task<void> writeStoredTemporaryWebSocketPayload(
    ruvia::WebSocket& socket) {
    auto operation = socket.text(std::string("owned-payload"));
    co_await std::move(operation);
}

struct ImmediateBodySource final {
    ruvia::Task<std::optional<std::string_view>> read() {
        co_return std::string_view("must-not-read");
    }
};

ruvia::ScopedOperation<std::optional<std::string_view>> makeExpiredBodyRead() {
    ruvia::detail::BodyReaderBinding<ImmediateBodySource> binding;
    return binding.facade().read();
}

ruvia::Task<void> awaitExpiredBodyRead(
    ruvia::ScopedOperation<std::optional<std::string_view>>& operation,
    bool& rejected) {
    try {
        (void)co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::Task<void> endWithTrailers(ruvia::ResponseStreamWriter& writer) {
    const std::array<ruvia::HttpHeaderView, 2> trailers{
        ruvia::HttpHeaderView{"Digest", "sha-256=value"},
        ruvia::HttpHeaderView{"Server-Timing", "db;dur=7"}};
    co_await writer.end(trailers);
}

ruvia::Task<void> endWithExpiredTrailerSources(
    ruvia::ResponseStreamWriter& writer) {
    auto operation = [&] {
        std::string name = "X-Owned-Trailer";
        std::string value = "temporary-value";
        const std::array<ruvia::HttpHeaderView, 1> trailers{
            ruvia::HttpHeaderView{name, value}};
        return writer.end(trailers);
    }();
    co_await std::move(operation);
}

struct SuspendedBodySource final {
    explicit SuspendedBodySource(asio::io_context& io)
        : dispatcher(std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8)),
          worker(ruvia::detail::WorkerHandleAccess::make(dispatcher)),
          signal(worker) {}

    ruvia::Task<std::optional<std::string_view>> read() {
        co_await signal.wait();
        co_return std::nullopt;
    }

    std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher;
    ruvia::WorkerHandle worker;
    ruvia::detail::WorkerSignal signal;
};

ruvia::Task<void> completeBodyRead(
    ruvia::BodyReader& reader,
    bool& completed) {
    (void)co_await reader.read();
    completed = true;
}

ruvia::Task<void> rejectConcurrentBodyRead(
    ruvia::BodyReader& reader,
    bool& rejected) {
    try {
        (void)co_await reader.read();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

}  // namespace

RUVIA_TEST(body_reader_rejects_concurrent_consumers_of_one_borrowed_buffer) {
    asio::io_context io(1);
    auto binding =
        ruvia::detail::BodyReaderBinding<SuspendedBodySource>(io);
    bool firstCompleted = false;
    bool secondRejected = false;

    auto first = asio::co_spawn(
        io,
        ruvia::detail::taskAsAwaitable(
            completeBodyRead(binding.facade(), firstCompleted)),
        asio::use_future);
    auto second = asio::co_spawn(
        io,
        ruvia::detail::taskAsAwaitable(
            rejectConcurrentBodyRead(binding.facade(), secondRejected)),
        asio::use_future);
    asio::post(io, [&binding] { binding.reader().signal.notify(); });
    io.run();
    first.get();
    second.get();

    RUVIA_CHECK(firstCompleted);
    RUVIA_CHECK(secondRejected);
}

RUVIA_TEST(response_stream_writeln_emits_independent_lines) {
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

RUVIA_TEST(response_stream_stored_writeln_operations_own_independent_payloads) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(writeStoredLines(writer)),
        asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{2});
    RUVIA_CHECK_EQ(sink.writes[0], std::string("stored-first\n"));
    RUVIA_CHECK_EQ(sink.writes[1], std::string("stored-second\n"));
}

RUVIA_TEST(response_stream_cold_operation_rejects_after_capability_teardown) {
    CaptureStreamSink sink;
    auto operation = makeExpiredWrite(sink);
    bool rejected = false;

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(
            awaitExpiredWrite(operation, rejected)),
        asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK(rejected);
    RUVIA_CHECK(sink.writes.empty());
}

RUVIA_TEST(websocket_stored_operation_owns_temporary_payload) {
    CaptureWebSocket capture;
    auto socket = ruvia::detail::WebSocketAccess::make(
        &capture, &readSocket, &writeSocket, &closeSocket);
    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(
            writeStoredTemporaryWebSocketPayload(socket)),
        asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK_EQ(capture.writes.size(), std::size_t{1});
    RUVIA_CHECK_EQ(capture.writes[0], std::string("owned-payload"));
}

RUVIA_TEST(websocket_cold_operation_rejects_after_facade_teardown) {
    CaptureWebSocket capture;
    auto operation = makeExpiredWebSocketWrite(capture);
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(
            awaitExpiredWrite(operation, rejected)),
        asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(capture.writes.empty());
}

RUVIA_TEST(body_reader_cold_operation_rejects_after_facade_teardown) {
    auto operation = makeExpiredBodyRead();
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(
            awaitExpiredBodyRead(operation, rejected)),
        asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
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

RUVIA_TEST(response_stream_stored_end_owns_trailer_names_and_values) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);
    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(endWithExpiredTrailerSources(writer)),
        asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK_EQ(sink.trailers.size(), std::size_t{1});
    RUVIA_CHECK_EQ(
        sink.trailers[0],
        std::string("X-Owned-Trailer=temporary-value"));
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
    // A present-but-empty id resets the EventSource last-event-ID buffer.
    RUVIA_CHECK_EQ(
        render(ruvia::SseMessage{.id = std::string_view{}}),
        std::string("id:\n\n"));
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
    ResponseStreamState bound;
    CaptureStreamSink opaqueContextStorage;
    auto* opaqueContext =
        reinterpret_cast<ruvia::Context*>(&opaqueContextStorage);
    bound.bindContext(opaqueContext, &unusedStreamingHead);
    bool rebindRejected = false;
    try {
        bound.bindContext(opaqueContext, &unusedStreamingHead);
    } catch (const std::logic_error&) {
        rebindRejected = true;
    }
    RUVIA_CHECK(rebindRejected);
    ResponseStreamState detached;
    detached.bindContext(opaqueContext, &unusedStreamingHead);
    detached.releaseContext();
    bool detachedContextRejected = false;
    try {
        (void)detached.streamingHead();
    } catch (const std::logic_error&) {
        detachedContextRejected = true;
    }
    RUVIA_CHECK(detachedContextRejected);
    bound.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(
        ruvia::detail::ResponseStreamFraming::kHttp1Chunked,
        ruvia::HttpKnownMethod::kGet,
        200,
        ruvia::detail::ResponseTrailerIntent::kNone));
    bool committedContextReleased = false;
    try {
        (void)bound.streamingHead();
    } catch (const std::logic_error& error) {
        committedContextReleased =
            std::string_view(error.what()) ==
            "response stream is already committed";
    }
    RUVIA_CHECK(committedContextReleased);

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
    open.markEnded();  // terminal transition is idempotent
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

    // Transport failure is a terminal alternative, not a second flag that can
    // coexist with Ended. A post-commit abort retains the exact wire plan for
    // dispatch accounting, while an uncommitted abort cannot manufacture one.
    ResponseStreamState abortedOpen;
    abortedOpen.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(
        ruvia::detail::ResponseStreamFraming::kHttp1Chunked,
        ruvia::HttpKnownMethod::kGet,
        206,
        ruvia::detail::ResponseTrailerIntent::kNone));
    abortedOpen.markAborted();
    abortedOpen.markAborted();
    RUVIA_CHECK(abortedOpen.aborted());
    RUVIA_CHECK(!abortedOpen.ended());
    RUVIA_CHECK(abortedOpen.committed());
    RUVIA_CHECK_EQ(
        abortedOpen.commitPlan()->responseStatus(),
        std::uint16_t{206});
    bool endAfterAbort = false;
    try {
        abortedOpen.markEnded();
    } catch (const std::logic_error&) {
        endAfterAbort = true;
    }
    RUVIA_CHECK(endAfterAbort);

    ResponseStreamState abortedBeforeCommit;
    abortedBeforeCommit.markAborted();
    RUVIA_CHECK(abortedBeforeCommit.aborted());
    RUVIA_CHECK(!abortedBeforeCommit.committed());
    RUVIA_CHECK(!abortedBeforeCommit.ended());
    RUVIA_CHECK(abortedBeforeCommit.commitPlan() == nullptr);

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

RUVIA_TEST(scoped_capability_move_relinks_and_parent_close_expires_destination) {
    ruvia::detail::ScopedOperationScope scope;
    int expiredCount = 0;
    TestScopedCapability first(scope, expiredCount);
    TestScopedCapability moved(std::move(first));
    moved.use();
    scope.close();
    RUVIA_CHECK_EQ(expiredCount, 1);
    bool rejected = false;
    try {
        moved.use();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(scoped_capability_copy_relinks_and_early_destruction_unlinks) {
    int expiredCount = 0;
    ruvia::detail::ScopedOperationScope scope;
    TestScopedCapability source(scope, expiredCount);
    {
        TestScopedCapability destroyedEarly(source);
        destroyedEarly.use();
    }
    TestScopedCapability survivingCopy(source);
    scope.close();
    RUVIA_CHECK_EQ(expiredCount, 2);

    bool sourceRejected = false;
    try {
        source.use();
    } catch (const std::logic_error&) {
        sourceRejected = true;
    }
    RUVIA_CHECK(sourceRejected);
}

RUVIA_TEST(scoped_operation_parent_close_destroys_cold_frame_immediately) {
    ruvia::detail::ScopedOperationScope scope;
    bool destroyed = false;
    auto operation = ruvia::detail::makeScopedOperation(
        scope, coldFrameTask(ColdFrameProbe(destroyed)));
    RUVIA_CHECK(!destroyed);
    scope.close();
    RUVIA_CHECK(destroyed);
    (void)operation;
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
