#include "test_io_context.h"
#include "test_harness.h"
#include "http2_connection_fixture.h"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/coding/HttpAcceptEncoding.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http2/Http2BufferedResponseWrite.h"
#include "ruvia/web/detail/http2/Http2SansIoSendWindow.h"
#include "ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
#include "ruvia/web/detail/http2/Http2SansIoWsTransport.h"
#include "ruvia/web/detail/router/RouteModes.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"

namespace {

#if !defined(_MSC_VER)
class ToggleRejectingMemoryResource final : public std::pmr::memory_resource {
public:
    void rejectAllocations(bool value) noexcept {
        rejecting_ = value;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (rejecting_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool rejecting_{false};
};
#endif  // !_MSC_VER

using ruvia::ProtocolByteLimit;
using ruvia::detail::Http2BufferedRequestBody;
using ruvia::detail::Http2RequestBodyRuntime;
using ruvia::detail::Http2SansIoBodyQueue;
using ruvia::detail::Http2SansIoResponseStreamSink;
using ruvia::detail::Http2SansIoStreamRuntime;
using ruvia::detail::Http2SansIoStreamRuntimeTable;
using ruvia::detail::Http2SansIoTermination;
using ruvia::detail::Http2SendWindowWaitResult;
using ruvia::detail::Http2StreamingRequestBody;
using ruvia::detail::Http2StreamState;
using ruvia::detail::HttpResponseCodingSelection;
using ruvia::detail::RequestBodyMode;
using ruvia::detail::RouteResolution;

ruvia::HttpResponse invalidStreamingHead(ruvia::Context&) {
    ruvia::HttpResponse response(std::pmr::new_delete_resource());
    response.header("Content-Length", "not-a-number");
    return response;
}

ruvia::HttpResponse okStreamingHead(ruvia::Context&) {
    return ruvia::HttpResponse(std::pmr::new_delete_resource());
}

[[nodiscard]] HttpResponseCodingSelection identityResponseCoding() {
    ruvia::detail::HttpResponseCodingQualities qualities;
    const auto selected = HttpResponseCodingSelection::select(qualities);
    if (selected.selected() == nullptr) {
        throw std::logic_error("identity response coding selection was empty");
    }
    return *selected.selected();
}

template <typename T>
concept HasDirectBodyModeSelection = requires(T& body) { body.selectMode(RequestBodyMode::kBuffered); };

template <typename T>
concept HasRawStreamIdAdmission = requires(T& table) { table.ensure(std::uint32_t{1}); };

template <typename T>
concept ExposesRvalueHttp2BodyQueuePop = requires(T&& queue) { std::move(queue).pop(); };

static_assert(!HasDirectBodyModeSelection<Http2RequestBodyRuntime>);
static_assert(!std::default_initializable<Http2RequestBodyRuntime>);
static_assert(sizeof(Http2RequestBodyRuntime) < sizeof(Http2BufferedRequestBody) + sizeof(Http2StreamingRequestBody));
static_assert(!HasRawStreamIdAdmission<Http2SansIoStreamRuntimeTable>);
static_assert(!ExposesRvalueHttp2BodyQueuePop<Http2SansIoBodyQueue>);
static_assert(std::same_as<decltype(std::declval<Http2SansIoStreamRuntimeTable&>().ensureAccepted(std::declval<const Http2StreamState&>())), Http2SansIoStreamRuntime&>);
static_assert(!std::default_initializable<Http2SendWindowWaitResult>);
static_assert(std::same_as<decltype(std::declval<const Http2SendWindowWaitResult&>().ready()), const ruvia::detail::Http2SendWindowReady*>);
static_assert(std::same_as<decltype(std::declval<const Http2SendWindowWaitResult&>().aborted()), const ruvia::detail::Http2SendWindowAborted*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http2SansIoSleepAwaiter&>().await_resume()), ruvia::TimerSleepResult>);
static_assert(std::constructible_from<Http2SansIoResponseStreamSink, ruvia::detail::Http2Connection&, std::uint32_t, ruvia::detail::ResponseStreamKind, const ruvia::WorkerHandle&, ruvia::detail::WorkerSignal&, ruvia::detail::Http2SansIoStreamSignal&, std::pmr::memory_resource*, ruvia::HttpKnownMethod, HttpResponseCodingSelection, ruvia::detail::HttpResponseCodingAvailability>);
static_assert(!std::constructible_from<Http2SansIoResponseStreamSink, ruvia::detail::Http2Connection&, std::uint32_t, ruvia::detail::ResponseStreamKind, const ruvia::WorkerHandle&, ruvia::detail::WorkerSignal&, ruvia::detail::Http2SansIoStreamSignal&, std::pmr::memory_resource*, ruvia::HttpKnownMethod, HttpResponseCodingSelection>);
static_assert(!std::constructible_from<Http2SansIoResponseStreamSink, ruvia::detail::Http2Connection&, std::uint32_t, ruvia::detail::ResponseStreamKind, ruvia::WorkerHandle&&, ruvia::detail::WorkerSignal&, ruvia::detail::Http2SansIoStreamSignal&, std::pmr::memory_resource*, ruvia::HttpKnownMethod, HttpResponseCodingSelection, ruvia::detail::HttpResponseCodingAvailability>);

Http2SansIoStreamRuntime& ensureAcceptedRuntime(Http2SansIoStreamRuntimeTable& table, std::uint32_t streamId, std::pmr::memory_resource* resource) {
    Http2StreamState acceptedStream(streamId, resource);
    return table.ensureAccepted(acceptedStream);
}

asio::awaitable<void> collectSendWindowResult(ruvia::detail::Http2Connection& connection, std::optional<Http2SendWindowWaitResult>& result) {
    result = co_await ruvia::detail::taskAsAwaitable(ruvia::detail::awaitHttp2SendWindow(connection, 1, nullptr));
}

}  // namespace

RUVIA_TEST(http2_send_window_wait_rejects_missing_stream_or_signal) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    ruvia::detail::Http2Connection connection(std::pmr::get_default_resource());
    std::optional<Http2SendWindowWaitResult> result;
    asio::co_spawn(io, collectSendWindowResult(connection, result), asio::detached);
    io.run();
    RUVIA_CHECK(result.has_value());
    RUVIA_CHECK(result->ready() == nullptr);
    RUVIA_CHECK(result->aborted() != nullptr);
}

RUVIA_TEST(http2_stream_sleep_reports_elapsed_result) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    Http2SansIoTermination termination;
    std::optional<ruvia::TimerSleepResult> observed;
    const auto waitForElapsed = [&]() -> ruvia::Task<ruvia::TimerSleepResult> {
        co_return co_await ruvia::detail::Http2SansIoSleepAwaiter(worker, termination, std::chrono::milliseconds(0));
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            observed = co_await ruvia::detail::taskAsAwaitable(waitForElapsed());
        },
        asio::detached);
    io.run();

    RUVIA_CHECK(observed.has_value());
    RUVIA_CHECK_EQ(*observed, ruvia::TimerSleepResult::kElapsed);
}

RUVIA_TEST(http2_worker_shutdown_reports_typed_sleep_result) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    Http2SansIoTermination termination;
    std::optional<ruvia::TimerSleepResult> observed;
    const auto waitForShutdown = [&]() -> ruvia::Task<ruvia::TimerSleepResult> {
        co_return co_await ruvia::detail::Http2SansIoSleepAwaiter(worker, termination, std::chrono::hours(1));
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            observed = co_await ruvia::detail::taskAsAwaitable(waitForShutdown());
        },
        asio::detached);
    asio::post(io, [&dispatcher] { dispatcher->stopTimers(); });
    io.run();

    RUVIA_CHECK(observed.has_value());
    RUVIA_CHECK_EQ(*observed, ruvia::TimerSleepResult::kStopRequested);
}

RUVIA_TEST(http2_session_termination_cancels_stream_sleep_with_exact_error) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    Http2SansIoTermination termination;
    std::error_code observed;
    const auto waitForTermination = [&]() -> ruvia::Task<ruvia::TimerSleepResult> {
        co_return co_await ruvia::detail::Http2SansIoSleepAwaiter(worker, termination, std::chrono::hours(1));
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::detail::taskAsAwaitable(waitForTermination());
            } catch (const std::system_error& error) {
                observed = error.code();
            }
        },
        asio::detached);
    asio::post(io, [&termination] { (void)termination.terminate(std::make_error_code(std::errc::connection_reset)); });
    io.run();

    RUVIA_CHECK_EQ(observed, std::make_error_code(std::errc::connection_reset));
}

RUVIA_TEST(http2_stream_head_failure_aborts_precommit_state) {
    using http2_connection_test::driveGetRequest;
    using http2_connection_test::handshake;

    std::pmr::monotonic_buffer_resource resource;
    ruvia::detail::Http2Connection connection(&resource);
    handshake(connection);
    driveGetRequest(connection, &resource);

    asio::io_context& io = ruvia::test::newTestIoContext();
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::detail::WorkerSignal writeSignal(worker);
    ruvia::detail::Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamSignal streamSignal(worker, termination);

    ruvia::WorkerMemory workerMemory;
    ruvia::RequestMemory requestMemory(workerMemory);
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ruvia::detail::ContextAccess::make(requestMemory, request);

    Http2SansIoResponseStreamSink sink(
        connection,
        1,
        ruvia::detail::ResponseStreamKind::kGeneric,
        worker,
        writeSignal,
        streamSignal,
        &resource,
        ruvia::HttpKnownMethod::kGet,
        identityResponseCoding(),
        ruvia::detail::HttpResponseCodingAvailability::kIdentityOnly);
    sink.bindContext(&context, &invalidStreamingHead);

    bool firstFailed = false;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::detail::taskAsAwaitable(sink.write("body"));
            } catch (const std::exception&) {
                firstFailed = true;
            }
        },
        asio::detached);
    io.run();

    RUVIA_CHECK(firstFailed);
    RUVIA_CHECK(!sink.committed());
    RUVIA_CHECK(sink.aborted());
    RUVIA_CHECK(connection.stream(1) != nullptr);
    if (const auto* stream = connection.stream(1)) {
        RUVIA_CHECK(stream->localSend().headPending() != nullptr);
    }

    // A failed head is terminal even though no HEADERS were emitted. The
    // second attempt must not reach the compression object's "already
    // prepared" state or manufacture a different representation.
    bool retryRejected = false;
    io.restart();
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::detail::taskAsAwaitable(sink.write("retry"));
            } catch (const std::logic_error&) {
                retryRejected = true;
            }
        },
        asio::detached);
    io.run();
    RUVIA_CHECK(retryRejected);
}

RUVIA_TEST(http2_response_stream_empty_end_is_idempotent_after_late_termination) {
    using http2_connection_test::driveGetRequest;
    using http2_connection_test::handshake;

    std::pmr::monotonic_buffer_resource resource;
    ruvia::detail::Http2Connection connection(&resource);
    handshake(connection);
    driveGetRequest(connection, &resource);

    asio::io_context& io = ruvia::test::newTestIoContext();
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::detail::WorkerSignal writeSignal(worker);
    ruvia::detail::Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamSignal streamSignal(worker, termination);

    ruvia::WorkerMemory workerMemory;
    ruvia::RequestMemory requestMemory(workerMemory);
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ruvia::detail::ContextAccess::make(requestMemory, request);

    Http2SansIoResponseStreamSink sink(
        connection,
        1,
        ruvia::detail::ResponseStreamKind::kGeneric,
        worker,
        writeSignal,
        streamSignal,
        &resource,
        ruvia::HttpKnownMethod::kGet,
        identityResponseCoding(),
        ruvia::detail::HttpResponseCodingAvailability::kIdentityOnly);
    sink.bindContext(&context, &okStreamingHead);

    bool firstEndCompleted = false;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            co_await ruvia::detail::taskAsAwaitable(sink.end({}));
            firstEndCompleted = true;
        },
        asio::detached);
    io.run();

    RUVIA_CHECK(firstEndCompleted);
    RUVIA_CHECK(sink.committed());

    (void)termination.terminate(std::make_error_code(std::errc::connection_reset));

    bool secondEndCompleted = false;
    bool secondEndRejected = false;
    io.restart();
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::detail::taskAsAwaitable(sink.end({}));
                secondEndCompleted = true;
            } catch (const std::system_error&) {
                secondEndRejected = true;
            }
        },
        asio::detached);
    io.run();

    RUVIA_CHECK(!secondEndRejected);
    RUVIA_CHECK(secondEndCompleted);
}

RUVIA_TEST(http2_web_body_queue_preserves_fifo_and_tracks_backlog) {
    Http2SansIoBodyQueue queue(std::pmr::get_default_resource());
    RUVIA_CHECK(queue.empty());
    queue.enqueue("first");
    queue.enqueue("second");
    queue.enqueue("third");
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{16});
    const auto active = queue.pop();
    RUVIA_CHECK_EQ(active, std::string_view("first"));
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{11});
    queue.enqueue("fourth");
    RUVIA_CHECK_EQ(active, std::string_view("first"));
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("second"));
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("third"));
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("fourth"));
    RUVIA_CHECK(queue.empty());
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{0});
}

RUVIA_TEST(http2_web_body_queue_reuses_storage_and_ignores_empty_chunks) {
    Http2SansIoBodyQueue queue(std::pmr::get_default_resource());
    queue.enqueue({});
    RUVIA_CHECK(queue.empty());
    for (int i = 0; i < 50; ++i) {
        queue.enqueue(std::to_string(i));
    }
    for (int i = 0; i < 50; ++i) {
        const auto expected = std::to_string(i);
        RUVIA_CHECK_EQ(queue.pop(), std::string_view(expected));
    }
    RUVIA_CHECK(queue.empty());
    queue.enqueue("reused");
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("reused"));
}

#if !defined(_MSC_VER)
// The queue probe injects failure through PMR string growth; MSVC's debug
// implementation does not complete that synthetic throwing path.
RUVIA_TEST(http2_web_body_queue_commits_backlog_only_after_storage_succeeds) {
    const std::string allocationSizedChunk(256, 'x');

    ToggleRejectingMemoryResource firstChunkResource;
    Http2SansIoBodyQueue emptyQueue(&firstChunkResource);
    firstChunkResource.rejectAllocations(true);
    bool firstChunkRejected = false;
    try {
        emptyQueue.enqueue(allocationSizedChunk);
    } catch (const std::bad_alloc&) {
        firstChunkRejected = true;
    }
    RUVIA_CHECK(firstChunkRejected);
    RUVIA_CHECK(emptyQueue.empty());
    RUVIA_CHECK_EQ(emptyQueue.queuedBytes(), std::size_t{0});

    ToggleRejectingMemoryResource overflowResource;
    Http2SansIoBodyQueue populatedQueue(&overflowResource);
    populatedQueue.enqueue("retained");
    overflowResource.rejectAllocations(true);
    bool overflowRejected = false;
    try {
        populatedQueue.enqueue(allocationSizedChunk);
    } catch (const std::bad_alloc&) {
        overflowRejected = true;
    }
    RUVIA_CHECK(overflowRejected);
    RUVIA_CHECK_EQ(populatedQueue.queuedBytes(), std::size_t{8});
    RUVIA_CHECK_EQ(populatedQueue.pop(), std::string_view("retained"));
    RUVIA_CHECK(populatedQueue.empty());
}

RUVIA_TEST(http2_websocket_transport_abort_remains_noexcept_when_reset_output_allocation_fails) {
    using http2_connection_test::driveGetRequest;
    using http2_connection_test::handshake;

    ToggleRejectingMemoryResource resource;
    ruvia::detail::Http2Connection connection(&resource);
    handshake(connection);
    driveGetRequest(connection, &resource);

    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    ruvia::detail::WorkerSignal writeSignal(worker);
    ruvia::detail::Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamSignal streamSignal(worker, termination);
    ruvia::detail::Http2SansIoBodyQueue queue(&resource);
    ruvia::detail::Http2SansIoWsTransport<asio::any_io_executor> transport(
        connection,
        1,
        queue,
        streamSignal,
        writeSignal,
        asio::any_io_executor(io.get_executor()));

    std::pmr::string scratch(&resource);
    connection.takeOutput(scratch);
    char settings[ruvia::detail::kHttp2FrameHeaderBytes];
    ruvia::detail::http2EncodeFrameHeader(
        settings,
        0,
        ruvia::detail::Http2FrameType::kSettings,
        0,
        0);
    RUVIA_CHECK(
        connection.feed(std::string_view(settings, sizeof(settings))) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK_EQ(
        connection.pendingOutput().size(),
        static_cast<std::size_t>(ruvia::detail::kHttp2FrameHeaderBytes));

    resource.rejectAllocations(true);
    bool aborted = false;
    const auto postResult = worker.post([&] {
        transport.abort();
        aborted = true;
        attachment.stop();
    });
    RUVIA_CHECK(postResult.accepted());
    io.run();
    RUVIA_CHECK(aborted);
}

RUVIA_TEST(http2_buffered_response_writer_reports_failure_when_reset_output_allocation_fails) {
    using http2_connection_test::driveGetRequest;
    using http2_connection_test::handshake;

    ToggleRejectingMemoryResource resource;
    ruvia::detail::Http2Connection connection(&resource);
    handshake(connection);
    driveGetRequest(connection, &resource);

    asio::io_context& io = ruvia::test::newTestIoContext();
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::detail::WorkerSignal writeSignal(worker);
    ruvia::detail::Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamRuntimeTable table(std::pmr::get_default_resource(), termination);
    ruvia::WorkerMemory workerMemory;
    ruvia::detail::Http2BufferedResponseWriter writer(connection, table, workerMemory, writeSignal);

    ruvia::HttpResponse response(std::pmr::get_default_resource());
    response.header("Connection", "close");
    const auto writePlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpKnownMethod::kGet, response);

    std::pmr::string scratch(&resource);
    connection.takeOutput(scratch);
    char settings[ruvia::detail::kHttp2FrameHeaderBytes];
    ruvia::detail::http2EncodeFrameHeader(
        settings,
        0,
        ruvia::detail::Http2FrameType::kSettings,
        0,
        0);
    RUVIA_CHECK(
        connection.feed(std::string_view(settings, sizeof(settings))) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK_EQ(
        connection.pendingOutput().size(),
        static_cast<std::size_t>(ruvia::detail::kHttp2FrameHeaderBytes));

    resource.rejectAllocations(true);
    bool threw = false;
    std::optional<ruvia::detail::Http2BufferedResponseWriteResult> result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                result = co_await ruvia::detail::taskAsAwaitable(
                    writer.write(1, response, writePlan));
            } catch (const std::bad_alloc&) {
                threw = true;
            }
        },
        asio::detached);
    io.run();

    RUVIA_CHECK(!threw);
    RUVIA_CHECK(result.has_value());
    if (result.has_value()) {
        RUVIA_CHECK(result->failedBeforeCommit() != nullptr);
    }
    dispatcher->detachContext();
}
#endif  // !_MSC_VER

RUVIA_TEST(http2_web_route_selection_owns_exact_body_storage) {
    Http2SansIoStreamRuntime bufferedRuntime(1, std::pmr::get_default_resource());
    RUVIA_CHECK(bufferedRuntime.selectedRoute() == nullptr);
    RUVIA_CHECK(bufferedRuntime.selectRoute(RouteResolution{}, RequestBodyMode::kBuffered));
    auto* bufferedSelection = bufferedRuntime.selectedRoute();
    RUVIA_CHECK(bufferedSelection != nullptr);
    RUVIA_CHECK(bufferedSelection->resolution().notFound() != nullptr);
    auto& bufferedBody = bufferedSelection->body();
    auto* buffered = bufferedBody.buffered();
    RUVIA_CHECK(buffered != nullptr);
    RUVIA_CHECK(bufferedBody.streaming() == nullptr);
    RUVIA_CHECK(bufferedBody.mode() == RequestBodyMode::kBuffered);
    RUVIA_CHECK(!bufferedRuntime.selectRoute(RouteResolution{}, RequestBodyMode::kStream));
    const auto bufferedStore = bufferedBody.store("abc", ProtocolByteLimit::limited(3), 0);
    RUVIA_CHECK(bufferedStore.stored() != nullptr);
    RUVIA_CHECK_EQ(buffered->bytes(), std::string_view("abc"));
    RUVIA_CHECK_EQ(bufferedBody.receivedBytes(), std::size_t{3});

    Http2SansIoStreamRuntime streamingRuntime(3, std::pmr::get_default_resource());
    RUVIA_CHECK(streamingRuntime.selectRoute(RouteResolution{}, RequestBodyMode::kStream));
    auto& streamingBody = streamingRuntime.selectedRoute()->body();
    auto* streaming = streamingBody.streaming();
    RUVIA_CHECK(streaming != nullptr);
    RUVIA_CHECK(streamingBody.buffered() == nullptr);
    const auto firstStreamingStore = streamingBody.store("one", ProtocolByteLimit::unlimited(), 8);
    RUVIA_CHECK(firstStreamingStore.stored() != nullptr);
    const auto secondStreamingStore = streamingBody.store("two", ProtocolByteLimit::unlimited(), 8);
    RUVIA_CHECK(secondStreamingStore.stored() != nullptr);
    RUVIA_CHECK_EQ(streaming->queue().queuedBytes(), std::size_t{6});
}

RUVIA_TEST(http2_web_request_body_runtime_enforces_total_and_backlog_limits) {
    Http2SansIoStreamRuntime bufferedRuntime(1, std::pmr::get_default_resource());
    RUVIA_CHECK(bufferedRuntime.selectRoute(RouteResolution{}, RequestBodyMode::kBuffered));
    auto& bufferedBody = bufferedRuntime.selectedRoute()->body();
    auto* buffered = bufferedBody.buffered();
    const auto bufferedStored = bufferedBody.store("1234", ProtocolByteLimit::limited(5), 0);
    RUVIA_CHECK(bufferedStored.stored() != nullptr);
    const auto totalLimitFailure = bufferedBody.store("67", ProtocolByteLimit::limited(5), 0);
    RUVIA_CHECK(totalLimitFailure.protocolFailure() != nullptr);
    if (const auto* failure = totalLimitFailure.protocolFailure()) {
        RUVIA_CHECK_EQ(failure->protocolError().status(), ruvia::http_status::kContentTooLarge);
    }
    RUVIA_CHECK_EQ(bufferedBody.receivedBytes(), std::size_t{4});
    RUVIA_CHECK_EQ(buffered->bytes(), std::string_view("1234"));

    Http2SansIoStreamRuntime streamingRuntime(3, std::pmr::get_default_resource());
    RUVIA_CHECK(streamingRuntime.selectRoute(RouteResolution{}, RequestBodyMode::kStream));
    auto& streamingBody = streamingRuntime.selectedRoute()->body();
    auto* streaming = streamingBody.streaming();
    const auto streamingStored = streamingBody.store("1234", ProtocolByteLimit::unlimited(), 5);
    RUVIA_CHECK(streamingStored.stored() != nullptr);
    const auto backlogOverflow = streamingBody.store("67", ProtocolByteLimit::unlimited(), 5);
    RUVIA_CHECK(backlogOverflow.backlogOverflow() != nullptr);
    RUVIA_CHECK_EQ(streamingBody.receivedBytes(), std::size_t{4});
    RUVIA_CHECK_EQ(streaming->queue().pop(), std::string_view("1234"));
    const auto resumedStore = streamingBody.store("67", ProtocolByteLimit::unlimited(), 5);
    RUVIA_CHECK(resumedStore.stored() != nullptr);
}

RUVIA_TEST(http2_web_stream_runtime_table_keeps_active_storage_stable) {
    std::pmr::monotonic_buffer_resource resource;
    Http2SansIoTermination termination;
    Http2SansIoStreamRuntimeTable table(&resource, termination);
    auto& first = ensureAcceptedRuntime(table, 1, &resource);
    RUVIA_CHECK(first.selectRoute(RouteResolution{}, RequestBodyMode::kBuffered));
    auto& firstBodyRuntime = first.selectedRoute()->body();
    const auto firstStore = firstBodyRuntime.store("tiny", ProtocolByteLimit::limited(16), 0);
    RUVIA_CHECK(firstStore.stored() != nullptr);
    const auto firstBody = firstBodyRuntime.buffered()->bytes();
    const auto* firstAddress = &first;

    // Cross the inline capacity so pointer-vector growth and later compaction are
    // both exercised without moving active runtime objects.
    for (std::uint32_t id = 3; id < 45; id += 2) {
        (void)ensureAcceptedRuntime(table, id, &resource);
    }
    RUVIA_CHECK(table.find(1) == firstAddress);
    RUVIA_CHECK_EQ(table.find(1)->selectedRoute()->body().buffered()->bytes(), firstBody);
    RUVIA_CHECK(table.remove(19));
    RUVIA_CHECK(table.find(1) == firstAddress);
    RUVIA_CHECK(!table.remove(19));
    RUVIA_CHECK(table.size() == 21);
}

RUVIA_TEST(http2_web_stream_runtime_table_owns_dispatch_signal_and_lease) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    std::pmr::monotonic_buffer_resource resource;
    Http2SansIoTermination termination;
    Http2SansIoStreamRuntimeTable table(&resource, termination);

    RUVIA_CHECK(table.beginDispatch(1, worker) == nullptr);
    auto& runtime = ensureAcceptedRuntime(table, 1, &resource);
    RUVIA_CHECK(!runtime.dispatched());
    RUVIA_CHECK(runtime.signal() == nullptr);
    RUVIA_CHECK_EQ(table.dispatchedCount(), std::size_t{0});
    RUVIA_CHECK(table.beginDispatch(1, worker) == nullptr);
    RUVIA_CHECK(runtime.selectRoute(RouteResolution{}, RequestBodyMode::kBuffered));
    auto* selectedRoute = runtime.selectedRoute();
    RUVIA_CHECK(selectedRoute != nullptr);
    RUVIA_CHECK(selectedRoute->signal() == nullptr);

    auto* signal = table.beginDispatch(1, worker);
    RUVIA_CHECK(signal != nullptr);
    RUVIA_CHECK(runtime.selectedRoute() == selectedRoute);
    RUVIA_CHECK(selectedRoute->dispatched());
    RUVIA_CHECK(selectedRoute->signal() == signal);
    RUVIA_CHECK(runtime.dispatched());
    RUVIA_CHECK(runtime.signal() == signal);
    RUVIA_CHECK_EQ(table.dispatchedCount(), std::size_t{1});
    RUVIA_CHECK(table.beginDispatch(1, worker) == nullptr);
    RUVIA_CHECK_EQ(table.dispatchedCount(), std::size_t{1});

    std::size_t visited = 0;
    table.forEach([&](const auto& entry) {
        ++visited;
        RUVIA_CHECK(entry.streamId() == std::uint32_t{1});
        RUVIA_CHECK(entry.dispatched());
    });
    RUVIA_CHECK_EQ(visited, std::size_t{1});

    asio::post(io, [signal, &termination] {
        signal->wake();
        (void)termination.terminate(std::make_error_code(std::errc::connection_aborted));
    });
    io.run();
    RUVIA_CHECK(signal->terminated());
    RUVIA_CHECK(table.remove(1));
    RUVIA_CHECK_EQ(table.dispatchedCount(), std::size_t{0});
    RUVIA_CHECK_EQ(table.size(), std::size_t{0});
}

RUVIA_TEST(http2_web_stream_signal_wakes_concurrent_waiters_without_self_cancel) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    std::pmr::monotonic_buffer_resource resource;
    Http2SansIoTermination termination;
    Http2SansIoStreamRuntimeTable table(&resource, termination);
    auto& runtime = ensureAcceptedRuntime(table, 1, &resource);
    RUVIA_CHECK(runtime.selectRoute(RouteResolution{}, RequestBodyMode::kBuffered));
    auto* signal = table.beginDispatch(1, worker);
    RUVIA_CHECK(signal != nullptr);
    if (signal == nullptr) {
        return;
    }

    std::size_t wakeCount = 0;
    const auto waitOnce = [&]() -> asio::awaitable<void> {
        co_await ruvia::detail::taskAsAwaitable(signal->wait());
        ++wakeCount;
    };
    asio::co_spawn(io, waitOnce(), asio::detached);
    asio::co_spawn(io, waitOnce(), asio::detached);
    (void)io.poll();
    RUVIA_CHECK_EQ(wakeCount, std::size_t{0});

    io.restart();
    asio::post(io, [signal] { signal->wake(); });
    io.run();
    RUVIA_CHECK_EQ(wakeCount, std::size_t{2});
}

RUVIA_TEST(http2_web_stream_runtime_keeps_overflow_signal_reference_stable) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    std::pmr::monotonic_buffer_resource resource;
    Http2SansIoTermination termination;
    Http2SansIoStreamRuntimeTable table(&resource, termination);
    for (std::uint32_t id = 1; id <= 33; id += 2) {
        (void)ensureAcceptedRuntime(table, id, &resource);
    }
    auto* runtime = table.find(33);
    RUVIA_CHECK(runtime != nullptr);
    if (runtime == nullptr) {
        return;
    }
    RUVIA_CHECK(runtime->selectRoute(RouteResolution{}, RequestBodyMode::kBuffered));
    auto* signal = table.beginDispatch(33, worker);
    RUVIA_CHECK(signal != nullptr);
    const auto* runtimeAddress = runtime;

    for (std::uint32_t id = 35; id <= 99; id += 2) {
        (void)ensureAcceptedRuntime(table, id, &resource);
    }
    RUVIA_CHECK(table.find(33) == runtimeAddress);
    RUVIA_CHECK(table.find(33)->signal() == signal);
    RUVIA_CHECK_EQ(table.dispatchedCount(), std::size_t{1});
    RUVIA_CHECK(table.remove(35));
    RUVIA_CHECK(table.find(33) == runtimeAddress);
    RUVIA_CHECK(table.find(33)->signal() == signal);
    RUVIA_CHECK(table.remove(33));
    RUVIA_CHECK_EQ(table.dispatchedCount(), std::size_t{0});
}
