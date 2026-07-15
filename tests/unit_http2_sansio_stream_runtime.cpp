#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/web/detail/http2/Http2SansIoSendWindow.h"
#include "ruvia/web/detail/router/RouteModes.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"

namespace {

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

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool rejecting_{false};
};

using ruvia::ProtocolByteLimit;
using ruvia::detail::Http2RequestBodyRuntime;
using ruvia::detail::Http2BufferedRequestBody;
using ruvia::detail::Http2StreamingRequestBody;
using ruvia::detail::Http2SansIoBodyQueue;
using ruvia::detail::Http2SansIoStreamRuntime;
using ruvia::detail::Http2SansIoStreamRuntimeTable;
using ruvia::detail::Http2SendWindowWaitResult;
using ruvia::detail::Http2StreamState;
using ruvia::detail::RequestBodyMode;
using ruvia::detail::RouteResolution;

template <typename T>
concept HasDirectBodyModeSelection = requires(T& body) {
    body.selectMode(RequestBodyMode::kBuffered);
};

template <typename T>
concept HasRawStreamIdAdmission = requires(T& table) {
    table.ensure(std::uint32_t{1});
};

static_assert(!HasDirectBodyModeSelection<Http2RequestBodyRuntime>);
static_assert(!std::default_initializable<Http2RequestBodyRuntime>);
static_assert(
    sizeof(Http2RequestBodyRuntime) <
    sizeof(Http2BufferedRequestBody) + sizeof(Http2StreamingRequestBody));
static_assert(!HasRawStreamIdAdmission<Http2SansIoStreamRuntimeTable>);
static_assert(std::same_as<
    decltype(std::declval<Http2SansIoStreamRuntimeTable&>().ensureAccepted(
        std::declval<const Http2StreamState&>())),
    Http2SansIoStreamRuntime&>);
static_assert(!std::default_initializable<Http2SendWindowWaitResult>);
static_assert(std::same_as<
    decltype(std::declval<const Http2SendWindowWaitResult&>().ready()),
    const ruvia::detail::Http2SendWindowReady*>);
static_assert(std::same_as<
    decltype(std::declval<const Http2SendWindowWaitResult&>().aborted()),
    const ruvia::detail::Http2SendWindowAborted*>);

Http2SansIoStreamRuntime& ensureAcceptedRuntime(
    Http2SansIoStreamRuntimeTable& table,
    std::uint32_t streamId,
    std::pmr::memory_resource* resource) {
    Http2StreamState acceptedStream(streamId, resource);
    return table.ensureAccepted(acceptedStream);
}

asio::awaitable<void> collectSendWindowResult(
    ruvia::detail::Http2Connection& connection,
    std::optional<Http2SendWindowWaitResult>& result) {
    result = co_await ruvia::detail::taskAsAwaitable(
        ruvia::detail::awaitHttp2SendWindow(connection, 1, nullptr));
}

}  // namespace

RUVIA_TEST(http2_send_window_wait_rejects_missing_stream_or_signal) {
    asio::io_context io;
    ruvia::detail::Http2Connection connection(std::pmr::get_default_resource());
    std::optional<Http2SendWindowWaitResult> result;
    asio::co_spawn(
        io,
        collectSendWindowResult(connection, result),
        asio::detached);
    io.run();
    RUVIA_CHECK(result.has_value());
    RUVIA_CHECK(result->ready() == nullptr);
    RUVIA_CHECK(result->aborted() != nullptr);
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

RUVIA_TEST(http2_web_route_selection_owns_exact_body_storage) {
    Http2SansIoStreamRuntime bufferedRuntime(
        1, std::pmr::get_default_resource());
    RUVIA_CHECK(bufferedRuntime.selectedRoute() == nullptr);
    RUVIA_CHECK(bufferedRuntime.selectRoute(
        RouteResolution{}, RequestBodyMode::kBuffered));
    auto* bufferedSelection = bufferedRuntime.selectedRoute();
    RUVIA_CHECK(bufferedSelection != nullptr);
    RUVIA_CHECK(bufferedSelection->resolution().notFound() != nullptr);
    auto& bufferedBody = bufferedSelection->body();
    auto* buffered = bufferedBody.buffered();
    RUVIA_CHECK(buffered != nullptr);
    RUVIA_CHECK(bufferedBody.streaming() == nullptr);
    RUVIA_CHECK(bufferedBody.mode() == RequestBodyMode::kBuffered);
    RUVIA_CHECK(!bufferedRuntime.selectRoute(
        RouteResolution{}, RequestBodyMode::kStream));
    const auto bufferedStore = bufferedBody.store(
        "abc", ProtocolByteLimit::limited(3), 0);
    RUVIA_CHECK(bufferedStore.stored() != nullptr);
    RUVIA_CHECK_EQ(buffered->bytes(), std::string_view("abc"));
    RUVIA_CHECK_EQ(bufferedBody.receivedBytes(), std::size_t{3});

    Http2SansIoStreamRuntime streamingRuntime(
        3, std::pmr::get_default_resource());
    RUVIA_CHECK(streamingRuntime.selectRoute(
        RouteResolution{}, RequestBodyMode::kStream));
    auto& streamingBody = streamingRuntime.selectedRoute()->body();
    auto* streaming = streamingBody.streaming();
    RUVIA_CHECK(streaming != nullptr);
    RUVIA_CHECK(streamingBody.buffered() == nullptr);
    const auto firstStreamingStore = streamingBody.store(
        "one", ProtocolByteLimit::unlimited(), 8);
    RUVIA_CHECK(firstStreamingStore.stored() != nullptr);
    const auto secondStreamingStore = streamingBody.store(
        "two", ProtocolByteLimit::unlimited(), 8);
    RUVIA_CHECK(secondStreamingStore.stored() != nullptr);
    RUVIA_CHECK_EQ(streaming->queue().queuedBytes(), std::size_t{6});
}

RUVIA_TEST(http2_web_request_body_runtime_enforces_total_and_backlog_limits) {
    Http2SansIoStreamRuntime bufferedRuntime(
        1, std::pmr::get_default_resource());
    RUVIA_CHECK(bufferedRuntime.selectRoute(
        RouteResolution{}, RequestBodyMode::kBuffered));
    auto& bufferedBody = bufferedRuntime.selectedRoute()->body();
    auto* buffered = bufferedBody.buffered();
    const auto bufferedStored = bufferedBody.store(
        "1234", ProtocolByteLimit::limited(5), 0);
    RUVIA_CHECK(bufferedStored.stored() != nullptr);
    const auto totalLimitFailure = bufferedBody.store(
        "67", ProtocolByteLimit::limited(5), 0);
    RUVIA_CHECK(totalLimitFailure.protocolFailure() != nullptr);
    if (const auto* failure = totalLimitFailure.protocolFailure()) {
        RUVIA_CHECK_EQ(failure->protocolError().status(), 413);
    }
    RUVIA_CHECK_EQ(bufferedBody.receivedBytes(), std::size_t{4});
    RUVIA_CHECK_EQ(buffered->bytes(), std::string_view("1234"));

    Http2SansIoStreamRuntime streamingRuntime(
        3, std::pmr::get_default_resource());
    RUVIA_CHECK(streamingRuntime.selectRoute(
        RouteResolution{}, RequestBodyMode::kStream));
    auto& streamingBody = streamingRuntime.selectedRoute()->body();
    auto* streaming = streamingBody.streaming();
    const auto streamingStored = streamingBody.store(
        "1234", ProtocolByteLimit::unlimited(), 5);
    RUVIA_CHECK(streamingStored.stored() != nullptr);
    const auto backlogOverflow = streamingBody.store(
        "67", ProtocolByteLimit::unlimited(), 5);
    RUVIA_CHECK(backlogOverflow.backlogOverflow() != nullptr);
    RUVIA_CHECK_EQ(streamingBody.receivedBytes(), std::size_t{4});
    RUVIA_CHECK_EQ(streaming->queue().pop(), std::string_view("1234"));
    const auto resumedStore = streamingBody.store(
        "67", ProtocolByteLimit::unlimited(), 5);
    RUVIA_CHECK(resumedStore.stored() != nullptr);
}

RUVIA_TEST(http2_web_stream_runtime_table_keeps_active_storage_stable) {
    std::pmr::monotonic_buffer_resource resource;
    Http2SansIoStreamRuntimeTable table(&resource);
    auto& first = ensureAcceptedRuntime(table, 1, &resource);
    RUVIA_CHECK(first.selectRoute(
        RouteResolution{}, RequestBodyMode::kBuffered));
    auto& firstBodyRuntime = first.selectedRoute()->body();
    const auto firstStore = firstBodyRuntime.store(
        "tiny", ProtocolByteLimit::limited(16), 0);
    RUVIA_CHECK(firstStore.stored() != nullptr);
    const auto firstBody = firstBodyRuntime.buffered()->bytes();
    const auto* firstAddress = &first;

    // Cross the inline capacity so pointer-vector growth and later compaction are
    // both exercised without moving active runtime objects.
    for (std::uint32_t id = 3; id < 45; id += 2) {
        (void)ensureAcceptedRuntime(table, id, &resource);
    }
    RUVIA_CHECK(table.find(1) == firstAddress);
    RUVIA_CHECK_EQ(
        table.find(1)->selectedRoute()->body().buffered()->bytes(),
        firstBody);
    RUVIA_CHECK(table.remove(19));
    RUVIA_CHECK(table.find(1) == firstAddress);
    RUVIA_CHECK(!table.remove(19));
    RUVIA_CHECK(table.size() == 21);
}

RUVIA_TEST(http2_web_stream_runtime_table_owns_dispatch_signal_and_lease) {
    asio::io_context io;
    std::pmr::monotonic_buffer_resource resource;
    Http2SansIoStreamRuntimeTable table(&resource);

    RUVIA_CHECK(table.beginDispatch(1, io.get_executor()) == nullptr);
    auto& runtime = ensureAcceptedRuntime(table, 1, &resource);
    RUVIA_CHECK(!runtime.dispatched());
    RUVIA_CHECK(runtime.signal() == nullptr);
    RUVIA_CHECK_EQ(table.dispatchedCount(), std::size_t{0});
    RUVIA_CHECK(table.beginDispatch(1, io.get_executor()) == nullptr);
    RUVIA_CHECK(runtime.selectRoute(
        RouteResolution{}, RequestBodyMode::kBuffered));
    auto* selectedRoute = runtime.selectedRoute();
    RUVIA_CHECK(selectedRoute != nullptr);
    RUVIA_CHECK(selectedRoute->signal() == nullptr);

    auto* signal = table.beginDispatch(1, io.get_executor());
    RUVIA_CHECK(signal != nullptr);
    RUVIA_CHECK(runtime.selectedRoute() == selectedRoute);
    RUVIA_CHECK(selectedRoute->dispatched());
    RUVIA_CHECK(selectedRoute->signal() == signal);
    RUVIA_CHECK(runtime.dispatched());
    RUVIA_CHECK(runtime.signal() == signal);
    RUVIA_CHECK_EQ(table.dispatchedCount(), std::size_t{1});
    RUVIA_CHECK(table.beginDispatch(1, io.get_executor()) == nullptr);
    RUVIA_CHECK_EQ(table.dispatchedCount(), std::size_t{1});

    std::size_t visited = 0;
    table.forEach([&](const auto& entry) {
        ++visited;
        RUVIA_CHECK(entry.streamId() == std::uint32_t{1});
        RUVIA_CHECK(entry.dispatched());
    });
    RUVIA_CHECK_EQ(visited, std::size_t{1});

    signal->wake();
    RUVIA_CHECK(!signal->ended());
    signal->end();
    RUVIA_CHECK(signal->ended());
    RUVIA_CHECK(table.remove(1));
    RUVIA_CHECK_EQ(table.dispatchedCount(), std::size_t{0});
    RUVIA_CHECK_EQ(table.size(), std::size_t{0});
}

RUVIA_TEST(http2_web_stream_signal_wakes_concurrent_waiters_without_self_cancel) {
    asio::io_context io;
    std::pmr::monotonic_buffer_resource resource;
    Http2SansIoStreamRuntimeTable table(&resource);
    auto& runtime = ensureAcceptedRuntime(table, 1, &resource);
    RUVIA_CHECK(runtime.selectRoute(
        RouteResolution{}, RequestBodyMode::kBuffered));
    auto* signal = table.beginDispatch(1, io.get_executor());
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

    signal->wake();
    io.restart();
    io.run();
    RUVIA_CHECK_EQ(wakeCount, std::size_t{2});
}

RUVIA_TEST(http2_web_stream_runtime_keeps_overflow_signal_reference_stable) {
    asio::io_context io;
    std::pmr::monotonic_buffer_resource resource;
    Http2SansIoStreamRuntimeTable table(&resource);
    for (std::uint32_t id = 1; id <= 33; id += 2) {
        (void)ensureAcceptedRuntime(table, id, &resource);
    }
    auto* runtime = table.find(33);
    RUVIA_CHECK(runtime != nullptr);
    if (runtime == nullptr) {
        return;
    }
    RUVIA_CHECK(runtime->selectRoute(
        RouteResolution{}, RequestBodyMode::kBuffered));
    auto* signal = table.beginDispatch(33, io.get_executor());
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
