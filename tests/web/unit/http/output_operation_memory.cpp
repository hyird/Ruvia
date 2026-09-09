#include <array>
#include <chrono>
#include <coroutine>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/WebSocket.h"
#include "ruvia/web/detail/http/StreamingAccess.h"
#include "ruvia/web/detail/websocket/WebSocketAccess.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

struct OutputSink final {
    std::vector<std::string> writes;
    std::vector<std::string> trailers;
    bool suspendNext{false};
    bool failNext{false};
    bool failAfterSuspend{false};
    const char* expectedStorage{nullptr};
    bool retainedStorage{false};
    std::coroutine_handle<> continuation{};
};

struct Suspend final {
    OutputSink& sink;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> continuation) noexcept {
        sink.continuation = continuation;
    }

    void await_resume() const noexcept {}
};

ruvia::Task<void> writeOutput(void* target, std::string_view value) {
    auto& sink = *static_cast<OutputSink*>(target);
    sink.retainedStorage = value.data() == sink.expectedStorage;
    sink.writes.emplace_back(value);
    if (std::exchange(sink.failNext, false)) {
        throw std::runtime_error("output failed");
    }
    if (std::exchange(sink.suspendNext, false)) {
        co_await Suspend{sink};
        if (std::exchange(sink.failAfterSuspend, false)) {
            throw std::runtime_error("output failed after suspension");
        }
    }
}

ruvia::Task<void> endOutput(void* target, std::span<const ruvia::HttpHeaderView> trailers) {
    auto& sink = *static_cast<OutputSink*>(target);
    for (const auto trailer : trailers) {
        sink.trailers.emplace_back(std::string(trailer.name()) + "=" + std::string(trailer.value()));
    }
    co_return;
}

ruvia::Task<ruvia::TimerSleepResult> sleepOutput(
    void*, std::chrono::milliseconds, const ruvia::StopToken&) {
    co_return ruvia::TimerSleepResult::kElapsed;
}

void bindOutput(void*, ruvia::Context*, ruvia::HttpResponse (*)(ruvia::Context&)) noexcept {}
void releaseOutput(void*) noexcept {}
bool outputFalse(void*) noexcept {
    return false;
}

ruvia::ResponseStreamWriter makeWriter(
    OutputSink& sink, std::pmr::memory_resource& resource) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(resource, &sink, &writeOutput,
        &endOutput, &sleepOutput, &bindOutput, &releaseOutput, &outputFalse, &outputFalse);
}

ruvia::Task<void> writeMany(ruvia::ResponseStreamWriter& writer) {
    for (int i = 0; i < 16; ++i) {
        co_await writer.write(std::string(256, static_cast<char>('a' + i)));
    }
}

struct WebSocketSink final {
    std::vector<std::string> writes;
    std::string closeReason;
    bool failNext{false};
};

ruvia::Task<std::optional<ruvia::WebSocketMessage>> readWebSocket(void*) {
    co_return std::nullopt;
}

ruvia::Task<void> writeWebSocket(void* target, ruvia::WebSocketOpcode, std::string_view payload) {
    auto& sink = *static_cast<WebSocketSink*>(target);
    sink.writes.emplace_back(payload);
    if (std::exchange(sink.failNext, false)) {
        throw std::runtime_error("websocket output failed");
    }
    co_return;
}

ruvia::Task<void> closeWebSocket(void* target, ruvia::WebSocketCloseOptions options) {
    static_cast<WebSocketSink*>(target)->closeReason = std::string(options.reason.view());
    co_return;
}

ruvia::WebSocket makeWebSocket(
    WebSocketSink& sink, std::pmr::memory_resource& resource) noexcept {
    return ruvia::detail::WebSocketAccess::make(resource, &sink, &readWebSocket,
        &writeWebSocket, &closeWebSocket);
}

ruvia::Task<void> awaitOperation(ruvia::ScopedOperation<void>& operation) {
    co_await std::move(operation);
}

ruvia::Task<void> writeManyWebSocket(ruvia::WebSocket& socket) {
    for (int i = 0; i < 16; ++i) {
        co_await socket.text(std::string(256, static_cast<char>('a' + i)));
    }
}

void run(asio::io_context& context, ruvia::Task<void> operation) {
    context.restart();
    auto future = asio::co_spawn(context,
        ruvia::detail::taskAsAwaitable(std::move(operation)), asio::use_future);
    context.run();
    future.get();
}

}  // namespace

RUVIA_TEST(output_operations_return_owner_allocations_after_repeated_success) {
    ruvia::test::CountingMemoryResource owner;
    OutputSink sink;
    auto writer = makeWriter(sink, owner);
    asio::io_context context(1);

    run(context, writeMany(writer));

    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{16});
}

RUVIA_TEST(output_failed_operation_returns_owner_allocations) {
    ruvia::test::CountingMemoryResource owner;
    OutputSink sink;
    sink.failNext = true;
    auto writer = makeWriter(sink, owner);
    asio::io_context context(1);
    bool failed = false;
    try {
        run(context, writeMany(writer));
    } catch (const std::runtime_error&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(output_cold_operation_discard_returns_owner_allocations) {
    ruvia::test::CountingMemoryResource owner;
    OutputSink sink;
    auto writer = makeWriter(sink, owner);
    {
        auto operation = writer.write(std::string(256, 'c'));
        RUVIA_CHECK(owner.liveAllocations() > 0);
    }
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(output_rvalue_move_reuses_compatible_owner_allocation) {
    ruvia::test::CountingMemoryResource owner;
    OutputSink sink;
    auto writer = makeWriter(sink, owner);
    asio::io_context context(1);

    {
        std::pmr::string payload(256, 'p', &owner);
        sink.expectedStorage = payload.data();
        auto operation = writer.write(std::move(payload));
        run(context, awaitOperation(operation));
        RUVIA_CHECK(sink.retainedStorage);
    }
    // A moved-from string can retain Debug STL metadata until its destruction.
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(sink.writes[0], std::string(256, 'p'));
}

RUVIA_TEST(output_rvalue_move_copies_incompatible_input_before_source_destruction) {
    ruvia::test::CountingMemoryResource owner;
    OutputSink sink;
    auto writer = makeWriter(sink, owner);
    asio::io_context context(1);

    auto operation = [&] {
        ruvia::test::CountingMemoryResource source;
        std::pmr::string payload(256, 'q', &source);
        return writer.write(std::move(payload));
    }();

    run(context, awaitOperation(operation));
    RUVIA_CHECK_EQ(sink.writes[0], std::string(256, 'q'));
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(output_sse_trailers_and_websocket_close_snapshot_inputs) {
    ruvia::test::CountingMemoryResource owner;
    OutputSink streamSink;
    auto writer = makeWriter(streamSink, owner);
    auto sse = ruvia::detail::StreamingAccess::makeSseWriter(writer);
    asio::io_context context(1);
    std::string data = "event-data";
    std::string event = "event-name";
    auto sseOperation = sse.write(ruvia::SseMessage{.data = data, .event = event});
    data.assign("changed-data");
    event.assign("changed-event");
    run(context, awaitOperation(sseOperation));

    std::string trailerName = "X-Output-Trailer";
    std::string trailerValue = "snapshot";
    const std::array<ruvia::HttpHeaderView, 1> trailers{
        ruvia::HttpHeaderView{trailerName, trailerValue}};
    auto trailerOperation = writer.end(trailers);
    trailerName.assign("changed-name");
    trailerValue.assign("changed-value");
    run(context, awaitOperation(trailerOperation));

    RUVIA_CHECK_EQ(streamSink.writes[0], std::string("event: event-name\ndata: event-data\n\n"));
    RUVIA_CHECK_EQ(streamSink.trailers[0], std::string("X-Output-Trailer=snapshot"));

    WebSocketSink socketSink;
    auto socket = makeWebSocket(socketSink, owner);
    std::string reason = "close-snapshot";
    auto closeOperation = socket.close({.code = 1000, .reason = reason});
    reason.assign("changed-reason");
    run(context, awaitOperation(closeOperation));
    RUVIA_CHECK_EQ(socketSink.closeReason, std::string("close-snapshot"));
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(output_pending_operation_returns_owner_allocation_after_resume) {
    ruvia::test::CountingMemoryResource owner;
    OutputSink sink;
    sink.suspendNext = true;
    auto writer = makeWriter(sink, owner);
    asio::io_context context(1);
    auto operation = writer.write(std::string(256, 's'));
    auto future = asio::co_spawn(context, ruvia::detail::taskAsAwaitable(awaitOperation(operation)), asio::use_future);
    context.poll();
    RUVIA_CHECK(owner.liveAllocations() > 0);
    sink.continuation.resume();
    context.run();
    future.get();
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(output_pending_failure_returns_owner_allocation_and_releases_lane) {
    ruvia::test::CountingMemoryResource owner;
    OutputSink sink;
    sink.suspendNext = true;
    sink.failAfterSuspend = true;
    auto writer = makeWriter(sink, owner);
    asio::io_context context(1);
    auto operation = writer.write(std::string(256, 'f'));
    auto future = asio::co_spawn(context, ruvia::detail::taskAsAwaitable(awaitOperation(operation)), asio::use_future);
    context.poll();
    RUVIA_CHECK(owner.liveAllocations() > 0);
    sink.continuation.resume();
    context.run();
    bool failed = false;
    try {
        future.get();
    } catch (const std::runtime_error&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});

    auto nextOperation = writer.write(std::string(256, 'r'));
    run(context, awaitOperation(nextOperation));
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(output_facade_destruction_reclaims_pending_writer_and_websocket_operations) {
    std::optional<ruvia::test::CountingMemoryResource> streamOwner(std::in_place);
    OutputSink streamSink;
    auto writerOperation = [&] {
        auto writer = makeWriter(streamSink, *streamOwner);
        return writer.write(std::string(256, 'w'));
    }();
    RUVIA_CHECK(streamOwner->allocationCount() > 0);
    RUVIA_CHECK_EQ(streamOwner->liveAllocations(), std::size_t{0});
    streamOwner.reset();

    std::optional<ruvia::test::CountingMemoryResource> socketOwner(std::in_place);
    WebSocketSink socketSink;
    auto websocketOperation = [&] {
        auto socket = makeWebSocket(socketSink, *socketOwner);
        return socket.text(std::string(256, 's'));
    }();
    RUVIA_CHECK(socketOwner->allocationCount() > 0);
    RUVIA_CHECK_EQ(socketOwner->liveAllocations(), std::size_t{0});
    socketOwner.reset();
}

RUVIA_TEST(websocket_output_operations_return_owner_allocations_after_repeated_success) {
    ruvia::test::CountingMemoryResource owner;
    WebSocketSink sink;
    auto socket = makeWebSocket(sink, owner);
    asio::io_context context(1);

    run(context, writeManyWebSocket(socket));

    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{16});
}

RUVIA_TEST(websocket_rvalue_move_copies_incompatible_input_before_source_destruction) {
    ruvia::test::CountingMemoryResource owner;
    WebSocketSink sink;
    auto socket = makeWebSocket(sink, owner);
    asio::io_context context(1);

    auto operation = [&] {
        ruvia::test::CountingMemoryResource source;
        std::pmr::string payload(256, 'q', &source);
        return socket.text(std::move(payload));
    }();

    run(context, awaitOperation(operation));
    RUVIA_CHECK_EQ(sink.writes[0], std::string(256, 'q'));
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(websocket_failed_output_returns_owner_allocations) {
    ruvia::test::CountingMemoryResource owner;
    WebSocketSink sink;
    sink.failNext = true;
    auto socket = makeWebSocket(sink, owner);
    asio::io_context context(1);
    bool failed = false;
    try {
        run(context, writeManyWebSocket(socket));
    } catch (const std::runtime_error&) {
        failed = true;
    }

    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});

    auto nextOperation = socket.text(std::string(256, 'r'));
    run(context, awaitOperation(nextOperation));
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
}
