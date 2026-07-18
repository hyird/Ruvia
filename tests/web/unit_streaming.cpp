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
#include <type_traits>
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

template <typename Text>
concept AcceptsSseData = requires(Text&& text) {
    ruvia::SseMessage{.data = std::forward<Text>(text)};
};

template <typename Text>
concept AcceptsSseEvent = requires(Text&& text) {
    ruvia::SseMessage{.event = std::forward<Text>(text)};
};

template <typename Text>
concept AcceptsSseId = requires(Text&& text) {
    ruvia::SseMessage{.id = std::forward<Text>(text)};
};

template <typename Text>
concept AcceptsAnySseTextAssignment =
    requires(ruvia::SseMessage& message, Text&& text) {
        message.data = std::forward<Text>(text);
    } ||
    requires(ruvia::SseMessage& message, Text&& text) {
        message.event = std::forward<Text>(text);
    } ||
    requires(ruvia::SseMessage& message, Text&& text) {
        message.id = std::forward<Text>(text);
    };

template <typename Text>
concept AcceptsAllSseTextAssignments = requires(
    ruvia::SseMessage& message,
    Text&& text) {
    message.data = std::forward<Text>(text);
    message.event = std::forward<Text>(text);
    message.id = std::forward<Text>(text);
};

static_assert(!AcceptsSseData<std::string>);
static_assert(!AcceptsSseData<const std::string>);
static_assert(!AcceptsSseData<std::pmr::string>);
static_assert(AcceptsSseData<std::string&>);
static_assert(AcceptsSseData<std::pmr::string&>);
static_assert(AcceptsSseData<std::string_view>);
static_assert(!AcceptsSseEvent<std::string>);
static_assert(!AcceptsSseEvent<const std::string>);
static_assert(!AcceptsSseEvent<std::pmr::string>);
static_assert(AcceptsSseEvent<std::string&>);
static_assert(AcceptsSseEvent<std::pmr::string&>);
static_assert(AcceptsSseEvent<std::string_view>);
static_assert(!AcceptsSseId<std::string>);
static_assert(!AcceptsSseId<const std::string>);
static_assert(!AcceptsSseId<std::pmr::string>);
static_assert(AcceptsSseId<std::string&>);
static_assert(AcceptsSseId<std::pmr::string&>);
static_assert(AcceptsSseId<std::string_view>);
static_assert(!AcceptsAnySseTextAssignment<std::string>);
static_assert(!AcceptsAnySseTextAssignment<const std::string>);
static_assert(!AcceptsAnySseTextAssignment<std::pmr::string>);
static_assert(AcceptsAllSseTextAssignments<std::string&>);
static_assert(AcceptsAllSseTextAssignments<std::pmr::string&>);
static_assert(AcceptsAllSseTextAssignments<std::string_view>);
static_assert(std::is_aggregate_v<ruvia::SseMessage>);
constexpr ruvia::SseMessage kLiteralSseMessage{
    .data = "data",
    .event = "event",
    .id = "id"};
static_assert(kLiteralSseMessage.data->view() == "data");
static_assert(kLiteralSseMessage.event == "event");
static_assert("event" == kLiteralSseMessage.event);
static_assert(kLiteralSseMessage.id->view() == "id");

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

struct SuspendedStreamSink final {
    explicit SuspendedStreamSink(asio::io_context& io)
        : dispatcher(std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8)),
          worker(ruvia::detail::WorkerHandleAccess::make(dispatcher)),
          signal(worker) {}

    std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher;
    ruvia::WorkerHandle worker;
    ruvia::detail::WorkerSignal signal;
    std::vector<std::string> writes;
    std::size_t ends{0};
    bool suspendNextWrite{true};
    bool failNextWrite{false};
};

ruvia::Task<void> writeSuspendedStream(
    void* target,
    std::string_view chunk) {
    auto& sink = *static_cast<SuspendedStreamSink*>(target);
    sink.writes.emplace_back(chunk);
    if (std::exchange(sink.failNextWrite, false)) {
        throw std::runtime_error("stream write failed");
    }
    if (std::exchange(sink.suspendNextWrite, false)) {
        co_await sink.signal.wait();
    }
}

ruvia::Task<void> endSuspendedStream(
    void* target,
    std::span<const ruvia::HttpHeaderView>) {
    ++static_cast<SuspendedStreamSink*>(target)->ends;
    co_return;
}

ruvia::ResponseStreamWriter makeSuspendedWriter(
    SuspendedStreamSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(
        &sink,
        &writeSuspendedStream,
        &endSuspendedStream,
        &sleepStream,
        &bindContext,
        &releaseContext,
        &committed,
        &aborted);
}

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

ruvia::Task<void> completeStreamWrite(
    ruvia::ResponseStreamWriter& writer,
    std::string_view chunk,
    bool& completed) {
    co_await writer.write(chunk);
    completed = true;
}

ruvia::Task<void> rejectConcurrentStreamWrite(
    ruvia::ResponseStreamWriter& writer,
    bool& rejected) {
    try {
        co_await writer.write("overlap");
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::Task<void> rejectConcurrentStreamEnd(
    ruvia::ResponseStreamWriter& writer,
    bool& rejected) {
    try {
        co_await writer.end();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::Task<void> observeStreamWriteFailure(
    ruvia::ResponseStreamWriter& writer,
    bool& failed) {
    try {
        co_await writer.write("failed");
    } catch (const std::runtime_error&) {
        failed = true;
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

RUVIA_TEST(response_stream_rejects_overlapping_output_operations) {
    asio::io_context io(1);
    SuspendedStreamSink sink(io);
    auto writer = makeSuspendedWriter(sink);
    bool firstCompleted = false;
    bool writeRejected = false;
    bool endRejected = false;
    bool failureObserved = false;

    auto first = asio::co_spawn(
        io,
        ruvia::detail::taskAsAwaitable(
            completeStreamWrite(writer, "first", firstCompleted)),
        asio::use_future);
    io.poll();
    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{1});
    RUVIA_CHECK(!firstCompleted);

    io.restart();
    auto overlappingWrite = asio::co_spawn(
        io,
        ruvia::detail::taskAsAwaitable(
            rejectConcurrentStreamWrite(writer, writeRejected)),
        asio::use_future);
    auto overlappingEnd = asio::co_spawn(
        io,
        ruvia::detail::taskAsAwaitable(
            rejectConcurrentStreamEnd(writer, endRejected)),
        asio::use_future);
    io.poll();
    overlappingWrite.get();
    overlappingEnd.get();

    asio::post(io, [&sink] { sink.signal.notify(); });
    io.restart();
    io.run();
    first.get();

    RUVIA_CHECK(firstCompleted);
    RUVIA_CHECK(writeRejected);
    RUVIA_CHECK(endRejected);
    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{1});
    RUVIA_CHECK_EQ(sink.ends, std::size_t{0});

    sink.failNextWrite = true;
    io.restart();
    auto failing = asio::co_spawn(
        io,
        ruvia::detail::taskAsAwaitable(
            observeStreamWriteFailure(writer, failureObserved)),
        asio::use_future);
    io.run();
    failing.get();
    RUVIA_CHECK(failureObserved);

    io.restart();
    auto following = asio::co_spawn(
        io,
        ruvia::detail::taskAsAwaitable(
            completeStreamWrite(writer, "following", firstCompleted)),
        asio::use_future);
    io.run();
    following.get();
    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{3});
    if (sink.writes.size() >= 3) {
        RUVIA_CHECK_EQ(sink.writes[2], std::string("following"));
    }
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

RUVIA_TEST(sse_writer_distinguishes_absent_and_empty_data) {
    // WHATWG HTML 9.2.6: a block whose data buffer is empty must NOT dispatch,
    // while even an empty `data:` field appends LF and makes that buffer
    // non-empty. The API must preserve that presence distinction.
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
    // A present empty data value is an event: the data field first makes the
    // EventSource data buffer non-empty, then dispatch removes its final LF.
    RUVIA_CHECK_EQ(
        render(ruvia::SseMessage{.data = ""}),
        std::string("data: \n\n"));
    // A present-but-empty id resets the EventSource last-event-ID buffer.
    RUVIA_CHECK_EQ(
        render(ruvia::SseMessage{.id = std::string_view{}}),
        std::string("id:\n\n"));
    // Non-empty data is unaffected: data lines are still emitted.
    RUVIA_CHECK_EQ(render(ruvia::SseMessage{.data = "hi"}), std::string("data: hi\n\n"));
    // No absent-data frame carries a "data:" line.
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
        ruvia::http_status::kOk,
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
        ruvia::http_status::kMultiStatus,
        ruvia::detail::ResponseTrailerIntent::kNone));
    RUVIA_CHECK(open.commitPlan() != nullptr);
    RUVIA_CHECK_EQ(
        open.commitPlan()->responseStatus(),
        ruvia::http_status::kMultiStatus);
    RUVIA_CHECK(
        open.commitPlan()->framing() ==
        ruvia::detail::ResponseStreamFraming::kHttp1Chunked);
    bool recommitRejected = false;
    try {
        open.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(
            ruvia::detail::ResponseStreamFraming::kHttp2Frames,
            ruvia::HttpKnownMethod::kGet,
            ruvia::HttpStatusCode::fromValue(418),
            ruvia::detail::ResponseTrailerIntent::kNone));
    } catch (const std::logic_error&) {
        recommitRejected = true;
    }
    RUVIA_CHECK(recommitRejected);
    RUVIA_CHECK_EQ(
        open.commitPlan()->responseStatus(),
        ruvia::http_status::kMultiStatus);
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
        ruvia::http_status::kPartialContent,
        ruvia::detail::ResponseTrailerIntent::kNone));
    abortedOpen.markAborted();
    abortedOpen.markAborted();
    RUVIA_CHECK(abortedOpen.aborted());
    RUVIA_CHECK(!abortedOpen.ended());
    RUVIA_CHECK(abortedOpen.committed());
    RUVIA_CHECK_EQ(
        abortedOpen.commitPlan()->responseStatus(),
        ruvia::http_status::kPartialContent);
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

    // A suppressed body (e.g. HEAD, 204 or 304) still refuses to accept a body
    // chunk, but with the head-only completion signal: writing the body a GET
    // would have produced is correct handler behavior there, so dispatch must
    // be able to tell it apart from a post-end() sequencing bug.
    ResponseStreamState suppressed;
    suppressed.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(
        ruvia::detail::ResponseStreamFraming::kHttp1Chunked,
        ruvia::HttpKnownMethod::kHead,
        ruvia::http_status::kOk,
        ruvia::detail::ResponseTrailerIntent::kNone));
    bool bodyRejected = false;
    try {
        suppressed.ensureBodyAllowed();
    } catch (const ruvia::detail::ResponseStreamHeadOnlyComplete&) {
        bodyRejected = true;
    }
    RUVIA_CHECK(bodyRejected);

    // HTTP/2 can keep the same content-forbidden response open solely for a
    // terminal trailing-HEADERS block, without accidentally enabling DATA.
    ResponseStreamState trailersOnly;
    trailersOnly.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(
        ruvia::detail::ResponseStreamFraming::kHttp2Frames,
        ruvia::HttpKnownMethod::kHead,
        ruvia::http_status::kOk,
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
    response.status(ruvia::http_status::kCreated);
    auto plan = ruvia::detail::httpResponseStreamCommitPlan(
        ruvia::detail::ResponseStreamFraming::kHttp1Chunked,
        ruvia::HttpKnownMethod::kGet,
        ruvia::http_status::kAccepted,
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
