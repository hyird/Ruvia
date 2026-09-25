#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/Http2Types.h"
#include "ruvia/http/HttpAcceptEncoding.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http2/Http2BufferedResponseWrite.h"
#include "ruvia/web/detail/http2/Http2DataOutputBudget.h"
#include "ruvia/web/detail/http2/Http2SansIoRequestBody.h"
#include "ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
#include "ruvia/web/detail/http2/Http2SansIoSendWindow.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/web/detail/http2/Http2SansIoWsTransport.h"
#include "ruvia/web/detail/router/RouteModes.h"

#include "context_services_fixture.h"
#include "http2_connection_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{0};
    std::size_t deallocations{0};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

#if !defined(_MSC_VER)
class ToggleRejectingMemoryResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{0};
    std::size_t deallocations{0};

    void rejectAllocations(bool value) noexcept {
        rejecting_ = value;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (rejecting_) {
            throw std::bad_alloc();
        }
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool rejecting_{false};
};
#endif  // !_MSC_VER

using ruvia::HttpResponseCodingSelection;
using ruvia::ProtocolByteLimit;
using ruvia::detail::Http2BufferedRequestBody;
using ruvia::detail::Http2BufferedResponseWriter;
using ruvia::detail::Http2DataOutputBudget;
using ruvia::detail::Http2RequestBodyRuntime;
using ruvia::detail::Http2SansIoBodyQueue;
using ruvia::detail::Http2SansIoResponseStreamSink;
using ruvia::detail::Http2SansIoStreamRuntime;
using ruvia::detail::Http2SansIoStreamRuntimeTable;
using ruvia::detail::Http2SansIoTermination;
using ruvia::detail::Http2SendWindowWaitResult;
using ruvia::detail::Http2StreamingRequestBody;
using ruvia::detail::RequestBodyMode;
using ruvia::detail::RouteResolution;

ruvia::Task<ruvia::HttpResponse> invalidStreamingHead(ruvia::Context&) {
    ruvia::HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.header("Content-Length", "not-a-number");
    co_return response;
}

ruvia::Task<ruvia::HttpResponse> okStreamingHead(ruvia::Context&) {
    co_return ruvia::HttpResponse({.resource = std::pmr::new_delete_resource()});
}

[[nodiscard]] HttpResponseCodingSelection identityResponseCoding() {
    ruvia::HttpResponseCodingQualities qualities;
    const auto selected = HttpResponseCodingSelection::select(qualities);
    if (selected.selected() == nullptr) {
        throw std::logic_error("identity response coding selection was empty");
    }
    return *selected.selected();
}

Http2SansIoStreamRuntime& ensureAcceptedRuntime(Http2SansIoStreamRuntimeTable& table,
    std::uint32_t streamId, std::pmr::memory_resource* resource) {
    (void)resource;
    return table.ensureAccepted(streamId);
}

void handshake(ruvia::Http2Connection& connection) {
    if (connection.feed(ruvia::detail::kHttp2ClientPreface) != ruvia::Http2FeedResult::kAccepted) {
        throw std::runtime_error("HTTP/2 server rejected preface");
    }
    char settings[ruvia::detail::kHttp2FrameHeaderBytes];
    ruvia::detail::http2EncodeFrameHeader(
        settings, 0, ruvia::Http2FrameType::kSettings, 0, 0);
    if (connection.feed(std::string_view(settings, sizeof(settings))) !=
        ruvia::Http2FeedResult::kAccepted) {
        throw std::runtime_error("HTTP/2 server rejected SETTINGS");
    }
    (void)connection.consumeOutput(connection.pendingOutput().size());
}

[[nodiscard]] ruvia::Http2RequestHeadEvent driveGetRequest(
    ruvia::Http2Connection& connection, std::pmr::memory_resource* resource) {
    std::pmr::string block(resource);
    http2_connection_test::encodeGetRequest(block);
    const auto frame = http2_connection_test::headersFrame(resource, 1,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));
    if (connection.feed(std::string_view(frame.data(), frame.size())) !=
        ruvia::Http2FeedResult::kAccepted) {
        throw std::runtime_error("HTTP/2 server rejected GET request");
    }
    std::optional<ruvia::Http2RequestHeadEvent> requestLease;
    while (auto event = connection.nextEvent()) {
        if (auto* requestHead = event->requestHead()) {
            requestLease.emplace(std::move(*requestHead));
        }
    }
    (void)connection.consumeOutput(connection.pendingOutput().size());
    if (!requestLease.has_value()) {
        throw std::runtime_error("HTTP/2 server emitted no request head");
    }
    return std::move(*requestLease);
}

auto stopIoOnCompletion(asio::io_context& io) {
    return [&io](std::exception_ptr) { asio::post(io, [&io] { io.stop(); }); };
}

asio::awaitable<void> collectSendWindowResult(
    ruvia::Http2Connection& connection, std::optional<Http2SendWindowWaitResult>& result) {
    result = co_await ruvia::asAwaitable(
        ruvia::detail::awaitHttp2SendWindow(connection, 1, nullptr));
}

asio::awaitable<void> acquireDataBudgetSlots(Http2DataOutputBudget& budget,
    const std::array<std::uint32_t, 4>& streamIds,
    ruvia::detail::Http2SansIoStreamSignal& signal, std::array<bool, 4>& acquired) {
    for (std::size_t i = 0; i < streamIds.size(); ++i) {
        acquired[i] = co_await ruvia::asAwaitable(
            budget.acquire(streamIds[i], signal));
    }
}

asio::awaitable<void> acquireDataBudgetSlot(Http2DataOutputBudget& budget,
    std::uint32_t streamId, ruvia::detail::Http2SansIoStreamSignal& signal, bool& acquired) {
    acquired = co_await ruvia::asAwaitable(budget.acquire(streamId, signal));
}

}  // namespace

RUVIA_TEST(http2_send_window_wait_rejects_missing_stream_or_signal) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto connection = ruvia::Http2Connection::server();
    std::optional<Http2SendWindowWaitResult> result;
    asio::co_spawn(io, collectSendWindowResult(connection, result), stopIoOnCompletion(io));
    io.run();
    io.restart();
    RUVIA_CHECK(result.has_value());
    RUVIA_CHECK(result->ready() == nullptr);
    RUVIA_CHECK(result->aborted() != nullptr);
}

RUVIA_TEST(http2_stream_sleep_reports_elapsed_result) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    Http2SansIoTermination termination;
    std::optional<ruvia::TimerSleepResult> observed;
    const auto waitForElapsed = [&]() -> ruvia::Task<ruvia::TimerSleepResult> {
        co_return co_await ruvia::detail::Http2SansIoSleepAwaiter(
            worker, termination, std::chrono::milliseconds(0));
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            observed = co_await ruvia::asAwaitable(waitForElapsed());
        },
        stopIoOnCompletion(io));
    io.run();
    io.restart();

    RUVIA_CHECK(observed.has_value());
    RUVIA_CHECK_EQ(*observed, ruvia::TimerSleepResult::kElapsed);
}

RUVIA_TEST(http2_worker_shutdown_reports_typed_sleep_result) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    Http2SansIoTermination termination;
    std::optional<ruvia::TimerSleepResult> observed;
    const auto waitForShutdown = [&]() -> ruvia::Task<ruvia::TimerSleepResult> {
        co_return co_await ruvia::detail::Http2SansIoSleepAwaiter(
            worker, termination, std::chrono::hours(1));
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            observed = co_await ruvia::asAwaitable(waitForShutdown());
        },
        stopIoOnCompletion(io));
    asio::post(io, [&attachment] { attachment.stop(); });
    io.run();
    io.restart();

    RUVIA_CHECK(observed.has_value());
    RUVIA_CHECK_EQ(*observed, ruvia::TimerSleepResult::kStopRequested);
}

RUVIA_TEST(http2_stream_sleep_observes_request_stop_token) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    Http2SansIoTermination termination;
    ruvia::StopSource source;
    auto token = source.token();
    std::optional<ruvia::TimerSleepResult> observed;
    const auto waitForStop = [&]() -> ruvia::Task<ruvia::TimerSleepResult> {
        co_return co_await ruvia::detail::Http2SansIoSleepAwaiter(
            worker, termination, std::chrono::hours(1), token);
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            observed = co_await ruvia::asAwaitable(waitForStop());
        },
        stopIoOnCompletion(io));
    asio::post(io, [&source] { source.requestStop(); });
    io.run();
    io.restart();

    RUVIA_CHECK(observed.has_value());
    RUVIA_CHECK_EQ(*observed, ruvia::TimerSleepResult::kStopRequested);
}

RUVIA_TEST(http2_stream_sleep_transfers_off_worker_stop_to_timer) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    Http2SansIoTermination termination;
    ruvia::StopSource source;
    std::optional<ruvia::TimerSleepResult> observed;
    const auto waitForStop = [&]() -> ruvia::Task<ruvia::TimerSleepResult> {
        co_return co_await ruvia::detail::Http2SansIoSleepAwaiter(
            worker, termination, std::chrono::hours(1), source.token());
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            observed = co_await ruvia::asAwaitable(waitForStop());
        },
        stopIoOnCompletion(io));
    (void)io.poll();
    std::thread requester([&source] { source.requestStop(); });
    requester.join();
    io.restart();
    io.run();
    io.restart();

    RUVIA_CHECK(observed.has_value());
    RUVIA_CHECK_EQ(*observed, ruvia::TimerSleepResult::kStopRequested);
}

RUVIA_TEST(http2_session_termination_cancels_stream_sleep_with_exact_error) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    Http2SansIoTermination termination;
    std::error_code observed;
    const auto waitForTermination = [&]() -> ruvia::Task<ruvia::TimerSleepResult> {
        co_return co_await ruvia::detail::Http2SansIoSleepAwaiter(
            worker, termination, std::chrono::hours(1));
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::asAwaitable(waitForTermination());
            } catch (const std::system_error& error) {
                observed = error.code();
            }
        },
        stopIoOnCompletion(io));
    asio::post(io, [&termination] {
        (void)termination.terminate(std::make_error_code(std::errc::connection_reset));
    });
    io.run();
    io.restart();

    RUVIA_CHECK_EQ(observed, std::make_error_code(std::errc::connection_reset));
}

RUVIA_TEST(http2_stream_head_failure_aborts_precommit_state) {
    std::pmr::monotonic_buffer_resource resource;
    auto connection = ruvia::Http2Connection::server({.resource = &resource});
    handshake(connection);
    [[maybe_unused]] auto requestLease = driveGetRequest(connection, &resource);

    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    ruvia::WorkerSignal writeSignal(worker);
    ruvia::detail::Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamSignal streamSignal(worker, termination);

    ruvia::WorkerMemory workerMemory;
    ruvia::RequestMemory requestMemory(workerMemory);
    auto [request, parseError] =
        ruvia::makeParsedHttpRequest("GET", "/", {}, {}, requestMemory.resource());
    RUVIA_CHECK(!parseError.has_value());
    auto context = ruvia::detail::ContextAccess::make(
        requestMemory, request, ruvia::test::testContextServices());

    Http2SansIoResponseStreamSink sink(connection, 1, ruvia::detail::ResponseStreamKind::kGeneric,
        writeSignal, streamSignal, &resource, ruvia::HttpKnownMethod::kGet,
        identityResponseCoding(), ruvia::detail::HttpResponseCodingAvailability::kIdentityOnly);
    sink.bindContext(&context, &invalidStreamingHead);

    bool firstFailed = false;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::asAwaitable(sink.write("body"));
            } catch (const std::exception&) {
                firstFailed = true;
            }
        },
        stopIoOnCompletion(io));
    io.run();
    io.restart();

    RUVIA_CHECK(firstFailed);
    RUVIA_CHECK(!sink.committed());
    RUVIA_CHECK(sink.aborted());
    RUVIA_CHECK_EQ(connection.streamReceiveStatus(1),
        ruvia::Http2StreamReceiveStatus::kEnded);

    // A failed head is terminal even though no HEADERS were emitted. The
    // second attempt must not reach the compression object's "already
    // prepared" state or manufacture a different representation.
    bool retryRejected = false;
    io.restart();
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::asAwaitable(sink.write("retry"));
            } catch (const std::logic_error&) {
                retryRejected = true;
            }
        },
        stopIoOnCompletion(io));
    io.run();
    io.restart();
    RUVIA_CHECK(retryRejected);
}

RUVIA_TEST(http2_response_stream_empty_end_is_idempotent_after_late_termination) {
    std::pmr::monotonic_buffer_resource resource;
    auto connection = ruvia::Http2Connection::server({.resource = &resource});
    handshake(connection);
    [[maybe_unused]] auto requestLease = driveGetRequest(connection, &resource);

    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    ruvia::WorkerSignal writeSignal(worker);
    ruvia::detail::Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamSignal streamSignal(worker, termination);

    ruvia::WorkerMemory workerMemory;
    ruvia::RequestMemory requestMemory(workerMemory);
    auto [request, parseError] =
        ruvia::makeParsedHttpRequest("GET", "/", {}, {}, requestMemory.resource());
    RUVIA_CHECK(!parseError.has_value());
    auto context = ruvia::detail::ContextAccess::make(
        requestMemory, request, ruvia::test::testContextServices());

    Http2SansIoResponseStreamSink sink(connection, 1, ruvia::detail::ResponseStreamKind::kGeneric,
        writeSignal, streamSignal, &resource, ruvia::HttpKnownMethod::kGet,
        identityResponseCoding(), ruvia::detail::HttpResponseCodingAvailability::kIdentityOnly);
    sink.bindContext(&context, &okStreamingHead);

    bool firstEndCompleted = false;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            co_await ruvia::asAwaitable(sink.end({}));
            firstEndCompleted = true;
        },
        stopIoOnCompletion(io));
    io.run();
    io.restart();

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
                co_await ruvia::asAwaitable(sink.end({}));
                secondEndCompleted = true;
            } catch (const std::system_error&) {
                secondEndRejected = true;
            }
        },
        stopIoOnCompletion(io));
    io.run();
    io.restart();

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
    RUVIA_CHECK_EQ(active, std::string_view("firstsecondthird"));
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{0});
    queue.enqueue("fourth");
    RUVIA_CHECK_EQ(active, std::string_view("firstsecondthird"));
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("fourth"));
    RUVIA_CHECK(queue.empty());
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{0});
}

RUVIA_TEST(http2_web_body_queue_reuses_storage_and_ignores_empty_chunks) {
    Http2SansIoBodyQueue queue(std::pmr::get_default_resource());
    queue.enqueue({});
    RUVIA_CHECK(queue.empty());
    std::string expected;
    for (int i = 0; i < 50; ++i) {
        const auto piece = std::to_string(i);
        expected += piece;
        queue.enqueue(piece);
    }
    RUVIA_CHECK_EQ(queue.pop(), std::string_view(expected));
    RUVIA_CHECK(queue.empty());
    queue.enqueue("reused");
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("reused"));
}

RUVIA_TEST(http2_web_body_queue_holds_data_credit_until_consumption_and_releases_storage) {
    auto connection = ruvia::Http2Connection::server();
    handshake(connection);
    std::pmr::string requestHead(std::pmr::get_default_resource());
    HpackEncoder::encodeHeader(requestHead, ":method", "POST");
    HpackEncoder::encodeHeader(requestHead, ":scheme", "https");
    HpackEncoder::encodeHeader(requestHead, ":path", "/stream");
    HpackEncoder::encodeHeader(requestHead, ":authority", "example.com");
    HpackEncoder::encodeHeader(requestHead, "content-length", "114688");
    const auto head = http2_connection_test::headersFrame(std::pmr::get_default_resource(), 1,
        ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(requestHead.data(), requestHead.size()));
    RUVIA_CHECK(connection.feed(std::string_view(head.data(), head.size())) ==
                ruvia::Http2FeedResult::kAccepted);
    std::optional<ruvia::Http2RequestHeadEvent> requestLease;
    while (auto event = connection.nextEvent()) {
        if (auto* value = event->requestHead()) {
            requestLease.emplace(std::move(*value));
        }
    }
    RUVIA_CHECK(requestLease.has_value());
    (void)connection.consumeOutput(connection.pendingOutput().size());

    CountingMemoryResource resource;
    {
        Http2SansIoBodyQueue queue(&resource);
        const std::string payload(16 * 1024, 'd');
        for (int round = 0; round != 2; ++round) {
            for (int frameIndex = 0; frameIndex != 3; ++frameIndex) {
                const auto frame = http2_connection_test::dataFrame(
                    std::pmr::get_default_resource(), 1, 0, payload);
                RUVIA_CHECK(connection.feed(std::string_view(frame.data(), frame.size())) ==
                            ruvia::Http2FeedResult::kAccepted);
                bool foundChunk = false;
                while (auto event = connection.nextEvent()) {
                    if (auto* data = event->messageBodyChunk()) {
                        const bool accepted = queue.enqueueBounded(
                            data->bytes(), data->takeCredit(), 2 * payload.size());
                        RUVIA_CHECK_EQ(accepted, frameIndex != 2);
                        foundChunk = true;
                    }
                }
                RUVIA_CHECK(foundChunk);
            }
            RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{2 * payload.size()});
            const auto active = queue.pop();
            RUVIA_CHECK_EQ(active.size(), std::size_t{2 * payload.size()});
            RUVIA_CHECK(active.front() == 'd' && active.back() == 'd');
            // The view remains valid while its DATA credits are returned.
            queue.releaseActiveCredits();
            RUVIA_CHECK(active.front() == 'd' && active.back() == 'd');
            (void)connection.consumeOutput(connection.pendingOutput().size());
        }

        const std::string exceptionPayload(16 * 1024, 'x');
        const auto frame = http2_connection_test::dataFrame(
            std::pmr::get_default_resource(), 1, 0, exceptionPayload);
        RUVIA_CHECK(connection.feed(std::string_view(frame.data(), frame.size())) ==
                    ruvia::Http2FeedResult::kAccepted);
        asio::io_context& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
        const auto worker = attachment.loop().handle();
        bool exceptionPathRan = false;
        {
            ruvia::detail::Http2SansIoTermination termination;
            ruvia::detail::Http2SansIoStreamSignal streamSignal(worker, termination);
            try {
                while (auto event = connection.nextEvent()) {
                    if (auto* data = event->messageBodyChunk()) {
                        Http2SansIoBodyQueue discarded(&resource);
                        discarded.enqueue(data->bytes(), data->takeCredit());
                        ruvia::detail::Http2SansIoRequestBodyReader reader(
                            connection, 1, discarded, streamSignal);
                        auto unstartedRead = reader.read();
                        (void)unstartedRead;
                        throw std::runtime_error("simulate handler failure after enqueue");
                    }
                }
            } catch (const std::runtime_error&) {
                exceptionPathRan = true;
            }
            RUVIA_CHECK(exceptionPathRan);
        }
        RUVIA_CHECK(resource.allocations > 0);
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

RUVIA_TEST(http2_web_body_queue_aggregates_one_byte_data_credits) {
    // HTTP/2 deliberately batches WINDOW_UPDATE until half the initial window
    // is consumed. Keep every DATA event's credit in the queue until the test
    // crosses that threshold instead of expecting a frame per returned byte.
    constexpr std::size_t kUpdateThreshold = 512 * 1024;
    constexpr std::size_t kOneByteFrames = 2048;
    auto connection = ruvia::Http2Connection::server();
    handshake(connection);
    std::pmr::string requestHead(std::pmr::get_default_resource());
    HpackEncoder::encodeHeader(requestHead, ":method", "POST");
    HpackEncoder::encodeHeader(requestHead, ":scheme", "https");
    HpackEncoder::encodeHeader(requestHead, ":path", "/stream");
    HpackEncoder::encodeHeader(requestHead, ":authority", "example.com");
    HpackEncoder::encodeHeader(requestHead, "content-length", std::to_string(4 * kUpdateThreshold));
    const auto head = http2_connection_test::headersFrame(std::pmr::get_default_resource(), 1,
        ruvia::detail::kHttp2FlagEndHeaders, requestHead);
    RUVIA_CHECK(connection.feed(head) == ruvia::Http2FeedResult::kAccepted);
    std::optional<ruvia::Http2RequestHeadEvent> requestLease;
    while (auto event = connection.nextEvent()) {
        if (auto* value = event->requestHead()) {
            requestLease.emplace(std::move(*value));
        }
    }
    RUVIA_CHECK(requestLease.has_value());
    (void)connection.consumeOutput(connection.pendingOutput().size());

    CountingMemoryResource resource;
    Http2SansIoBodyQueue queue(&resource);
    for (std::size_t i = 0; i < kOneByteFrames; ++i) {
        const auto frame = http2_connection_test::dataFrame(
            std::pmr::get_default_resource(), 1, 0, "x");
        RUVIA_CHECK(connection.feed(frame) == ruvia::Http2FeedResult::kAccepted);
        bool foundChunk = false;
        while (auto event = connection.nextEvent()) {
            if (auto* data = event->messageBodyChunk()) {
                RUVIA_CHECK(queue.enqueueBounded(
                    data->bytes(), data->takeCredit(), kUpdateThreshold));
                foundChunk = true;
            }
        }
        RUVIA_CHECK(foundChunk);
    }
    std::size_t remaining = kUpdateThreshold - kOneByteFrames;
    const std::string payload(16384, 'x');
    while (remaining != 0) {
        const auto count = std::min(remaining, payload.size());
        const auto frame = http2_connection_test::dataFrame(
            std::pmr::get_default_resource(), 1, 0, std::string_view(payload.data(), count));
        RUVIA_CHECK(connection.feed(frame) == ruvia::Http2FeedResult::kAccepted);
        bool foundChunk = false;
        while (auto event = connection.nextEvent()) {
            if (auto* data = event->messageBodyChunk()) {
                RUVIA_CHECK(queue.enqueueBounded(
                    data->bytes(), data->takeCredit(), kUpdateThreshold));
                foundChunk = true;
            }
        }
        RUVIA_CHECK(foundChunk);
        remaining -= count;
    }
    RUVIA_CHECK_EQ(queue.queuedBytes(), kUpdateThreshold);
    RUVIA_CHECK(resource.allocations <= 32);
    RUVIA_CHECK_EQ(connection.pendingOutput().size(), std::size_t{0});

    const auto active = queue.pop();
    RUVIA_CHECK_EQ(active.size(), kUpdateThreshold);
    RUVIA_CHECK_EQ(connection.pendingOutput().size(), std::size_t{0});
    queue.enqueue("next");
    RUVIA_CHECK_EQ(active.front(), 'x');
    RUVIA_CHECK_EQ(active.back(), 'x');
    RUVIA_CHECK_EQ(connection.pendingOutput().size(), std::size_t{0});
    (void)queue.pop();
    RUVIA_CHECK(connection.pendingOutput().size() > 0);
    (void)connection.consumeOutput(connection.pendingOutput().size());

    // Discarding queued credit returns it too, but WINDOW_UPDATE is thresholded.
    {
        Http2SansIoBodyQueue discarded(&resource);
        const std::string discardPayload(16384, 'y');
        for (std::size_t i = 0; i < kUpdateThreshold / discardPayload.size(); ++i) {
            const auto frame = http2_connection_test::dataFrame(
                std::pmr::get_default_resource(), 1, 0, discardPayload);
            RUVIA_CHECK(connection.feed(frame) == ruvia::Http2FeedResult::kAccepted);
            bool foundChunk = false;
            while (auto event = connection.nextEvent()) {
                if (auto* data = event->messageBodyChunk()) {
                    discarded.enqueue(data->bytes(), data->takeCredit());
                    foundChunk = true;
                }
            }
            RUVIA_CHECK(foundChunk);
        }
        RUVIA_CHECK_EQ(discarded.queuedBytes(), kUpdateThreshold);
        RUVIA_CHECK_EQ(connection.pendingOutput().size(), std::size_t{0});
    }
    RUVIA_CHECK(connection.pendingOutput().size() > 0);
    (void)connection.consumeOutput(connection.pendingOutput().size());

#if !defined(_MSC_VER)
    ToggleRejectingMemoryResource rejectingResource;
    {
        Http2SansIoBodyQueue failedQueue(&rejectingResource);
        const std::string failedPayload(16384, 'z');
        for (std::size_t i = 0; i < 32; ++i) {
            const auto frame = http2_connection_test::dataFrame(
                std::pmr::get_default_resource(), 1, 0, failedPayload);
            RUVIA_CHECK(connection.feed(frame) == ruvia::Http2FeedResult::kAccepted);
            while (auto event = connection.nextEvent()) {
                if (auto* data = event->messageBodyChunk()) {
                    failedQueue.enqueue(data->bytes(), data->takeCredit());
                }
            }
        }
        RUVIA_CHECK_EQ(failedQueue.queuedBytes(), kUpdateThreshold);

        const auto failedFrame = http2_connection_test::dataFrame(
            std::pmr::get_default_resource(), 1, 0, payload);
        RUVIA_CHECK(connection.feed(failedFrame) == ruvia::Http2FeedResult::kAccepted);
        bool appendFailed = false;
        while (auto event = connection.nextEvent()) {
            if (auto* data = event->messageBodyChunk()) {
                rejectingResource.rejectAllocations(true);
                try {
                    failedQueue.enqueue(data->bytes(), data->takeCredit());
                } catch (const std::bad_alloc&) {
                    appendFailed = true;
                }
            }
        }
        RUVIA_CHECK(appendFailed);
        RUVIA_CHECK_EQ(failedQueue.queuedBytes(), kUpdateThreshold);
        RUVIA_CHECK_EQ(failedQueue.pop().size(), kUpdateThreshold);
    }
    RUVIA_CHECK_EQ(rejectingResource.allocations, rejectingResource.deallocations);
    RUVIA_CHECK(connection.pendingOutput().size() > 0);
#endif
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

RUVIA_TEST(http2_websocket_transport_empty_end_completes_with_zero_send_window) {
    std::pmr::monotonic_buffer_resource resource;
    auto connection = ruvia::Http2Connection::server({.resource = &resource});
    handshake(connection);

    // Advertise an empty per-stream send window before admitting the stream.
    char peerSettings[ruvia::detail::kHttp2FrameHeaderBytes + 6]{};
    ruvia::detail::http2EncodeFrameHeader(peerSettings, 6,
        ruvia::Http2FrameType::kSettings, 0, 0);
    peerSettings[9] = 0;
    peerSettings[10] = 4;  // SETTINGS_INITIAL_WINDOW_SIZE
    RUVIA_CHECK(connection.feed(std::string_view(peerSettings, sizeof(peerSettings))) ==
                ruvia::Http2FeedResult::kAccepted);
    (void)connection.consumeOutput(connection.pendingOutput().size());

    [[maybe_unused]] auto requestLease = driveGetRequest(connection, &resource);
    const auto window = connection.sendWindowState(1);
    RUVIA_CHECK(window.has_value());
    if (window.has_value()) {
        RUVIA_CHECK_EQ(window->available, std::int32_t{0});
    }
    ruvia::HttpResponse response({.resource = &resource});
    const auto head = connection.submitStreamingResponseHead(1, std::move(response));
    RUVIA_CHECK(head == ruvia::Http2SubmitStatus::kAccepted);
    (void)connection.consumeOutput(connection.pendingOutput().size());
    RUVIA_CHECK(!connection.hasQueuedData(1));

    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    ruvia::WorkerSignal writeSignal(worker);
    ruvia::detail::Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamSignal streamSignal(worker, termination);
    ruvia::detail::Http2SansIoBodyQueue bodyQueue(&resource);
    ruvia::detail::Http2SansIoWsTransport<asio::any_io_executor> transport(
        connection, 1, bodyQueue, streamSignal, writeSignal, asio::any_io_executor(io.get_executor()));

    // A watchdog makes a regression fail instead of leaving this unit test hung.
    asio::steady_timer watchdog(io);
    watchdog.expires_after(std::chrono::seconds(1));
    bool timedOut = false;
    bool completed = false;
    std::error_code writeError;
    watchdog.async_wait([&](const std::error_code& error) {
        if (!error) {
            timedOut = true;
            (void)termination.terminate(std::make_error_code(std::errc::timed_out));
            streamSignal.wake();
            io.stop();
        }
    });
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            writeError = co_await ruvia::asAwaitable(transport.writeBytes(
                {}, ruvia::WebSocketServerTransportDisposition::kEndTransport));
            completed = true;
            watchdog.cancel();
            attachment.stop(); }, stopIoOnCompletion(io));
    io.run();
    io.restart();

    RUVIA_CHECK(completed);
    RUVIA_CHECK(!timedOut);
    RUVIA_CHECK(!writeError);
    RUVIA_CHECK(!connection.hasQueuedData(1));
    const auto output = connection.pendingOutput();
    const auto frame = ruvia::parseHttp2FrameHeader(std::span<const char>(output.data(), output.size()));
    RUVIA_CHECK(frame.has_value());
    if (frame.has_value()) {
        RUVIA_CHECK_EQ(frame->type, static_cast<std::uint8_t>(ruvia::Http2FrameType::kData));
        RUVIA_CHECK_EQ(frame->flags, ruvia::detail::kHttp2FlagEndStream);
        RUVIA_CHECK_EQ(frame->streamId, std::uint32_t{1});
        RUVIA_CHECK_EQ(frame->length, std::uint32_t{0});
    }
    attachment.stop();
}

RUVIA_TEST(http2_websocket_transport_abort_remains_noexcept_when_reset_output_allocation_fails) {
    ToggleRejectingMemoryResource resource;
    auto connection = ruvia::Http2Connection::server({.resource = &resource});
    handshake(connection);
    std::pmr::string requestBlock(&resource);
    http2_connection_test::encodeGetRequest(requestBlock);
    const auto requestFrame = http2_connection_test::headersFrame(&resource, 1,
        ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(requestBlock.data(), requestBlock.size()));
    RUVIA_CHECK(connection.feed(std::string_view(requestFrame.data(), requestFrame.size())) ==
                ruvia::Http2FeedResult::kAccepted);
    std::optional<ruvia::Http2RequestHeadEvent> requestLease;
    while (auto event = connection.nextEvent()) {
        if (auto* requestHead = event->requestHead()) {
            requestLease.emplace(std::move(*requestHead));
        }
    }
    RUVIA_CHECK(requestLease.has_value());
    (void)connection.consumeOutput(connection.pendingOutput().size());

    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    ruvia::WorkerSignal writeSignal(worker);
    ruvia::detail::Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamSignal streamSignal(worker, termination);
    ruvia::detail::Http2SansIoBodyQueue queue(&resource);
    ruvia::detail::Http2SansIoWsTransport<asio::any_io_executor> transport(
        connection, 1, queue, streamSignal, writeSignal, asio::any_io_executor(io.get_executor()));

    std::pmr::string scratch(&resource);
    connection.takeOutput(scratch);
    char settings[ruvia::detail::kHttp2FrameHeaderBytes];
    ruvia::detail::http2EncodeFrameHeader(
        settings, 0, ruvia::Http2FrameType::kSettings, 0, 0);
    RUVIA_CHECK(connection.feed(std::string_view(settings, sizeof(settings))) ==
                ruvia::Http2FeedResult::kAccepted);
    RUVIA_CHECK_EQ(connection.pendingOutput().size(),
        static_cast<std::size_t>(ruvia::detail::kHttp2FrameHeaderBytes));

    resource.rejectAllocations(true);
    bool aborted = false;
    bool readCompleted = false;
    std::optional<ruvia::detail::WsTransportReadResult> readResult;
    std::pmr::string readBuffer(&resource);
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            readResult.emplace(co_await ruvia::asAwaitable(transport.readMore(readBuffer)));
            readCompleted = true;
            if (aborted) {
                attachment.stop();
            } }, stopIoOnCompletion(io));
    const auto postResult = worker.post([&] {
        transport.abort();
        aborted = true;
        if (readCompleted) {
            attachment.stop();
        }
    });
    RUVIA_CHECK(postResult.accepted());
    asio::steady_timer watchdog(io);
    watchdog.expires_after(std::chrono::seconds(1));
    watchdog.async_wait([&](const std::error_code& error) {
        if (!error && !readCompleted) {
            io.stop();
        }
    });
    io.run();
    io.restart();
    RUVIA_CHECK(aborted);
    RUVIA_CHECK(readCompleted);
    RUVIA_CHECK(readResult.has_value());
    if (readResult.has_value()) {
        RUVIA_CHECK(readResult->failure() != nullptr);
    }
    attachment.stop();
}

RUVIA_TEST(http2_buffered_response_writer_reports_failure_when_reset_output_allocation_fails) {
    ToggleRejectingMemoryResource resource;
    auto connection = ruvia::Http2Connection::server({.resource = &resource});
    handshake(connection);
    [[maybe_unused]] auto requestLease = driveGetRequest(connection, &resource);

    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    ruvia::WorkerSignal writeSignal(worker);
    ruvia::detail::Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamRuntimeTable table(
        std::pmr::get_default_resource(), termination);
    ruvia::WorkerMemory workerMemory;
    ruvia::detail::Http2BufferedResponseWriter writer(connection, table, workerMemory, writeSignal);

    ruvia::HttpResponse response({.resource = std::pmr::get_default_resource()});
    response.header("Connection", "close");
    const auto writePlan =
        ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);

    std::pmr::string scratch(&resource);
    connection.takeOutput(scratch);
    char settings[ruvia::detail::kHttp2FrameHeaderBytes];
    ruvia::detail::http2EncodeFrameHeader(
        settings, 0, ruvia::Http2FrameType::kSettings, 0, 0);
    RUVIA_CHECK(connection.feed(std::string_view(settings, sizeof(settings))) ==
                ruvia::Http2FeedResult::kAccepted);
    RUVIA_CHECK_EQ(connection.pendingOutput().size(),
        static_cast<std::size_t>(ruvia::detail::kHttp2FrameHeaderBytes));

    resource.rejectAllocations(true);
    bool threw = false;
    std::optional<ruvia::detail::Http2BufferedResponseWriteResult> result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                result =
                    co_await ruvia::asAwaitable(writer.write(1, response, writePlan));
            } catch (const std::bad_alloc&) {
                threw = true;
            }
        },
        stopIoOnCompletion(io));
    io.run();
    io.restart();

    RUVIA_CHECK(!threw);
    RUVIA_CHECK(result.has_value());
    if (result.has_value()) {
        RUVIA_CHECK(result->failedBeforeCommit() != nullptr ||
                    result->failedAfterCommit() != nullptr);
    }
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

struct DataOutputObservation final {
    std::array<std::uint32_t, 8> streamIds{};
    std::array<std::size_t, 8> payloadBytes{};
    std::size_t count{0};
};

void observeDataOutput(void* context, std::uint32_t streamId, std::size_t payloadBytes) noexcept {
    auto& observation = *static_cast<DataOutputObservation*>(context);
    if (observation.count < observation.streamIds.size()) {
        observation.streamIds[observation.count] = streamId;
        observation.payloadBytes[observation.count] = payloadBytes;
        ++observation.count;
    }
}

RUVIA_TEST(http2_data_output_batch_observes_only_successfully_taken_data_frames) {
    auto connection = ruvia::Http2Connection::client();
    (void)connection.consumeOutput(connection.pendingOutput().size());
    char peerSettings[ruvia::detail::kHttp2FrameHeaderBytes];
    ruvia::detail::http2EncodeFrameHeader(
        peerSettings, 0, ruvia::Http2FrameType::kSettings, 0, 0);
    RUVIA_CHECK(connection.feed(std::string_view(peerSettings, sizeof(peerSettings))) ==
                ruvia::Http2FeedResult::kAccepted);

    const auto first = connection.submitRequestHead(ruvia::Http2RegularRequestHeadView{
        .method = "POST", .scheme = "https", .authority = "example.test", .target = "/a", .content = ruvia::Http2RequestContent::streaming()});
    const auto second = connection.submitRequestHead(ruvia::Http2RegularRequestHeadView{
        .method = "POST", .scheme = "https", .authority = "example.test", .target = "/b", .content = ruvia::Http2RequestContent::streaming()});
    RUVIA_CHECK(first.submitted() != nullptr);
    RUVIA_CHECK(second.submitted() != nullptr);
    if (!first.submitted() || !second.submitted()) {
        return;
    }
    const auto firstId = first.submitted()->streamId();
    const auto secondId = second.submitted()->streamId();
    RUVIA_CHECK(connection.submitData(firstId, "one", ruvia::Http2EndStream::kKeepOpen) ==
                ruvia::Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK(connection.submitData(secondId, "two!", ruvia::Http2EndStream::kKeepOpen) ==
                ruvia::Http2DataSubmitStatus::kAccepted);

    std::pmr::string output;
    output = "stale";
    output.clear();  // The Web wrapper clears reused scratch storage before batching.
    DataOutputObservation observation;
    const auto batch = connection.takeOutputBatch(1, output, observeDataOutput, &observation);
    RUVIA_CHECK(batch.status == ruvia::Http2OutputBatchStatus::kTaken);
    RUVIA_CHECK_EQ(output.size(), batch.bytes);
    RUVIA_CHECK(!output.empty());
    RUVIA_CHECK(observation.count == 0);  // leading SETTINGS ACK/HEADERS are control output

    while (connection.wantsWrite()) {
        const auto next = connection.takeOutputBatch(16 * 1024, output, observeDataOutput, &observation);
        RUVIA_CHECK(next.status == ruvia::Http2OutputBatchStatus::kTaken);
        if (next.status != ruvia::Http2OutputBatchStatus::kTaken) {
            break;
        }
    }
    RUVIA_CHECK_EQ(observation.count, std::size_t{2});
    if (observation.count == 2) {
        RUVIA_CHECK_EQ(observation.streamIds[0], firstId);
        RUVIA_CHECK_EQ(observation.payloadBytes[0], std::size_t{3});
        RUVIA_CHECK_EQ(observation.streamIds[1], secondId);
        RUVIA_CHECK_EQ(observation.payloadBytes[1], std::size_t{4});
    }
}

RUVIA_TEST(http2_data_output_budget_caps_slots_and_waits_for_core_drain) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    ruvia::Http2Connection connection = ruvia::Http2Connection::server();
    Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamSignal streamSignal(worker, termination);
    Http2DataOutputBudget budget(worker);

    // Cold acquisition has no side effect until started. The four accepted
    // reservations each represent at most one 16 KiB DATA frame, independent
    // of the connection's much larger protocol flow-control windows.
    auto cold = budget.acquire(99, streamSignal);
    (void)cold;
    std::size_t acquired = 0;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            for (const auto streamId : {1U, 3U, 5U, 7U}) {
                if (co_await ruvia::asAwaitable(
                        budget.acquire(streamId, streamSignal))) {
                    ++acquired;
                }
            }
            // A fifth stream cannot reserve another slot until one is released.
            bool fifthAcquired = false;
            asio::co_spawn(io,
                [&]() -> asio::awaitable<void> {
                    fifthAcquired = co_await ruvia::asAwaitable(
                        budget.acquire(9, streamSignal));
                }, stopIoOnCompletion(io));
            (void)io.poll();
            RUVIA_CHECK(!fifthAcquired);

            // Control output is not charged to DATA slots. A server SETTINGS
            // frame remains independently pending while all DATA credits are held.
            RUVIA_CHECK(!connection.pendingOutput().empty());
            budget.release(3);
            (void)io.poll();
            RUVIA_CHECK(fifthAcquired);
            RUVIA_CHECK_EQ(acquired, std::size_t{4});
            budget.release(1);
            budget.release(5);
            budget.release(7);
            budget.release(9); }, stopIoOnCompletion(io));
    io.run();
    io.restart();

    // Exercise the same ownership boundary with real serialized DATA from the
    // public connection API. Releasing a removed stream is not enough: its slot
    // stays held while bytes remain in core output and after they are handed to
    // the socket. Only the corresponding completed socket batch releases it.
    auto wireConnection = ruvia::Http2Connection::client();
    (void)wireConnection.consumeOutput(wireConnection.pendingOutput().size());
    char peerSettings[ruvia::detail::kHttp2FrameHeaderBytes];
    ruvia::detail::http2EncodeFrameHeader(
        peerSettings, 0, ruvia::Http2FrameType::kSettings, 0, 0);
    RUVIA_CHECK(wireConnection.feed(std::string_view(peerSettings, sizeof(peerSettings))) ==
                ruvia::Http2FeedResult::kAccepted);
    (void)wireConnection.consumeOutput(wireConnection.pendingOutput().size());
    Http2DataOutputBudget wireBudget(worker);
    std::array<std::uint32_t, 4> streamIds{};
    for (std::size_t i = 0; i < streamIds.size(); ++i) {
        const auto submitted = wireConnection.submitRequestHead(ruvia::Http2RegularRequestHeadView{
            .method = "POST", .scheme = "https", .authority = "example.test", .target = "/", .content = ruvia::Http2RequestContent::streaming()});
        RUVIA_CHECK(submitted.submitted() != nullptr);
        if (submitted.submitted() == nullptr) {
            continue;
        }
        streamIds[i] = submitted.submitted()->streamId();
        RUVIA_CHECK(wireConnection.submitData(streamIds[i], "data", ruvia::Http2EndStream::kKeepOpen) ==
                    ruvia::Http2DataSubmitStatus::kAccepted);
        wireBudget.noteDataSubmitted(streamIds[i], 4);
    }
    bool wireFifthAcquired = false;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            for (const auto id : streamIds) {
                RUVIA_CHECK(co_await ruvia::asAwaitable(
                    wireBudget.acquire(id, streamSignal)));
            }
            asio::co_spawn(io,
                [&]() -> asio::awaitable<void> {
                    wireFifthAcquired = co_await ruvia::asAwaitable(
                        wireBudget.acquire(99, streamSignal));
                }, stopIoOnCompletion(io));
            (void)io.poll();
            RUVIA_CHECK(!wireFifthAcquired);

            for (const auto id : streamIds) {
                wireBudget.release(id);
            }
            wireBudget.reconcile(wireConnection, false);
            (void)io.poll();
            RUVIA_CHECK(!wireFifthAcquired);

            // Consume leading HEADERS, stopping at the first actual DATA frame.
            while (true) {
                const auto pending = wireConnection.pendingOutput();
                const auto header = ruvia::parseHttp2FrameHeader(
                    std::span<const char>(pending.data(), pending.size()));
                RUVIA_CHECK(header.has_value());
                if (!header || header->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kData)) {
                    break;
                }
                RUVIA_CHECK(wireConnection.consumeOutput(ruvia::kHttp2FrameHeaderBytes + header->length) !=
                            ruvia::Http2OutputConsumeStatus::kOutOfRange);
            }

            for (const auto id : streamIds) {
                const auto pending = wireConnection.pendingOutput();
                const auto header = ruvia::parseHttp2FrameHeader(
                    std::span<const char>(pending.data(), pending.size()));
                RUVIA_CHECK(header.has_value());
                RUVIA_CHECK(header && header->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kData));
                if (!header || header->type != static_cast<std::uint8_t>(ruvia::Http2FrameType::kData)) {
                    break;
                }
                const auto frameBytes = ruvia::kHttp2FrameHeaderBytes + header->length;
                wireBudget.noteDataOutput(id, header->length);
                RUVIA_CHECK(wireConnection.consumeOutput(frameBytes) !=
                            ruvia::Http2OutputConsumeStatus::kOutOfRange);
                wireBudget.reconcile(wireConnection, false);
                (void)io.poll();
                RUVIA_CHECK(!wireFifthAcquired);
                wireBudget.reconcile(wireConnection, true);
                (void)io.poll();
                RUVIA_CHECK(wireFifthAcquired);
                wireBudget.release(99);
                wireFifthAcquired = false;
                if (id != streamIds.back()) {
                    asio::co_spawn(io,
                        [&]() -> asio::awaitable<void> {
                            wireFifthAcquired = co_await ruvia::asAwaitable(
                                wireBudget.acquire(99, streamSignal));
                        }, stopIoOnCompletion(io));
                    (void)io.poll();
                    RUVIA_CHECK(!wireFifthAcquired);
                }
            } }, stopIoOnCompletion(io));
    io.run();
    io.restart();
}

RUVIA_TEST(http2_data_output_budget_reconciles_discarded_queued_data_without_socket_output) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    auto connection = ruvia::Http2Connection::client();
    (void)connection.consumeOutput(connection.pendingOutput().size());
    char peerSettings[ruvia::detail::kHttp2FrameHeaderBytes + 6]{};
    ruvia::detail::http2EncodeFrameHeader(peerSettings, 6,
        ruvia::Http2FrameType::kSettings, 0, 0);
    peerSettings[9] = 0;
    peerSettings[10] = 4;
    RUVIA_CHECK(connection.feed(std::string_view(peerSettings, sizeof(peerSettings))) ==
                ruvia::Http2FeedResult::kAccepted);
    (void)connection.consumeOutput(connection.pendingOutput().size());

    const auto submitted = connection.submitRequestHead(ruvia::Http2RegularRequestHeadView{
        .method = "POST", .scheme = "https", .authority = "example.test", .target = "/", .content = ruvia::Http2RequestContent::streaming()});
    RUVIA_CHECK(submitted.submitted() != nullptr);
    const auto streamId = submitted.submitted() == nullptr ? std::uint32_t{0}
                                                           : submitted.submitted()->streamId();
    RUVIA_CHECK(connection.submitData(streamId, "queued", ruvia::Http2EndStream::kKeepOpen) ==
                ruvia::Http2DataSubmitStatus::kQueued);
    RUVIA_CHECK(connection.dataQueueState(streamId) == ruvia::Http2DataQueueState::kQueued);
    RUVIA_CHECK_EQ(connection.pendingDataOutputBytes(streamId), std::size_t{0});

    Http2DataOutputBudget budget(worker);
    Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamSignal streamSignal(worker, termination);
    const std::array<std::uint32_t, 4> heldIds{streamId, 5U, 7U, 9U};
    std::array<bool, 4> acquired{};
    asio::co_spawn(io, acquireDataBudgetSlots(budget, heldIds, streamSignal, acquired), stopIoOnCompletion(io));
    io.run();
    io.restart();
    for (const bool value : acquired) {
        RUVIA_CHECK(value);
    }
    for (const auto id : heldIds) {
        budget.noteDataSubmitted(id, id == streamId ? 6 : 1);
    }

    bool fifthAcquired = false;
    asio::co_spawn(io, acquireDataBudgetSlot(budget, 11, streamSignal, fifthAcquired), stopIoOnCompletion(io));
    (void)io.poll();
    io.restart();
    RUVIA_CHECK(!fifthAcquired);

    char reset[ruvia::detail::kHttp2FrameHeaderBytes + 4]{};
    ruvia::detail::http2EncodeFrameHeader(reset, 4,
        ruvia::Http2FrameType::kRstStream, 0, streamId);
    RUVIA_CHECK(connection.feed(std::string_view(reset, sizeof(reset))) ==
                ruvia::Http2FeedResult::kAccepted);
    RUVIA_CHECK_EQ(connection.pendingDataOutputBytes(streamId), std::size_t{0});
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        budget.releaseAndReconcile(streamId, connection);
        co_return; }, stopIoOnCompletion(io));
    for (std::size_t attempt = 0; attempt < 32 && !fifthAcquired; ++attempt) {
        (void)io.poll();
        io.restart();
    }
    RUVIA_CHECK(fifthAcquired);
    if (!fifthAcquired) {
        (void)termination.terminate(std::make_error_code(std::errc::operation_canceled));
        for (std::size_t attempt = 0; attempt < 32; ++attempt) {
            (void)io.poll();
            io.restart();
        }
        io.stop();
    }
}

RUVIA_TEST(http2_data_output_budget_recovers_after_peer_reset_without_reusing_pending_output) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    auto connection = ruvia::Http2Connection::client();
    (void)connection.consumeOutput(connection.pendingOutput().size());

    // Restrict the peer's initial stream window to one byte: the first byte is
    // serialized while the remainder of stream 1 stays queued in the core.
    char settings[ruvia::detail::kHttp2FrameHeaderBytes + 6]{};
    ruvia::detail::http2EncodeFrameHeader(settings, 6,
        ruvia::Http2FrameType::kSettings, 0, 0);
    settings[9] = 0;
    settings[10] = 4;
    settings[14] = 1;
    RUVIA_CHECK(connection.feed(std::string_view(settings, sizeof(settings))) ==
                ruvia::Http2FeedResult::kAccepted);
    (void)connection.consumeOutput(connection.pendingOutput().size());

    const auto firstResult = connection.submitRequestHead(ruvia::Http2RegularRequestHeadView{
        .method = "POST", .scheme = "https", .authority = "example.test", .target = "/", .content = ruvia::Http2RequestContent::streaming()});
    const auto secondResult = connection.submitRequestHead(ruvia::Http2RegularRequestHeadView{
        .method = "POST", .scheme = "https", .authority = "example.test", .target = "/", .content = ruvia::Http2RequestContent::streaming()});
    RUVIA_CHECK(firstResult.submitted() != nullptr);
    RUVIA_CHECK(secondResult.submitted() != nullptr);
    const auto first = firstResult.submitted() == nullptr ? std::uint32_t{0} : firstResult.submitted()->streamId();
    const auto second = secondResult.submitted() == nullptr ? std::uint32_t{0} : secondResult.submitted()->streamId();
    RUVIA_CHECK_EQ(first, std::uint32_t{1});
    RUVIA_CHECK_EQ(second, std::uint32_t{3});
    RUVIA_CHECK(connection.submitData(first, "ab", ruvia::Http2EndStream::kKeepOpen) ==
                ruvia::Http2DataSubmitStatus::kQueued);
    RUVIA_CHECK_EQ(connection.pendingDataOutputBytes(first), std::size_t{1});
    RUVIA_CHECK(connection.dataQueueState(first) == ruvia::Http2DataQueueState::kQueued);
    while (true) {
        const auto pending = connection.pendingOutput();
        const auto header = ruvia::parseHttp2FrameHeader(
            std::span<const char>(pending.data(), pending.size()));
        RUVIA_CHECK(header.has_value());
        if (!header || header->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kData)) {
            break;
        }
        RUVIA_CHECK(connection.consumeOutput(ruvia::kHttp2FrameHeaderBytes + header->length) !=
                    ruvia::Http2OutputConsumeStatus::kOutOfRange);
    }

    Http2DataOutputBudget budget(worker);
    Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamSignal streamSignal(worker, termination);
    const std::array<std::uint32_t, 4> heldIds{first, 5U, 7U, 9U};
    std::array<bool, 4> acquired{};
    asio::co_spawn(io, acquireDataBudgetSlots(budget, heldIds, streamSignal, acquired), stopIoOnCompletion(io));
    io.run();
    io.restart();
    for (std::size_t i = 0; i < acquired.size(); ++i) {
        RUVIA_CHECK(acquired[i]);
        budget.noteDataSubmitted(heldIds[i], 1);
    }

    budget.noteDataSubmitted(first, 2);
    bool secondAcquired = false;
    asio::co_spawn(io, acquireDataBudgetSlot(budget, second, streamSignal, secondAcquired),
        stopIoOnCompletion(io));
    (void)io.poll();
    io.restart();
    RUVIA_CHECK(!secondAcquired);

    // Peer RST discards the still-flow-controlled suffix, but cannot reclaim
    // the credit while the already serialized DATA frame remains in core output.
    char reset[ruvia::detail::kHttp2FrameHeaderBytes + 4]{};
    ruvia::detail::http2EncodeFrameHeader(reset, 4,
        ruvia::Http2FrameType::kRstStream, 0, first);
    RUVIA_CHECK(connection.feed(std::string_view(reset, sizeof(reset))) ==
                ruvia::Http2FeedResult::kAccepted);
    budget.noteDataOutput(first, 1);
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        budget.release(first);
        budget.reconcile(connection, false);
        co_return; }, stopIoOnCompletion(io));
    (void)io.poll();
    io.restart();
    RUVIA_CHECK(!secondAcquired);
    RUVIA_CHECK_EQ(connection.pendingDataOutputBytes(first), std::size_t{1});

    // Taking a complete batch records DATA as in-flight, but even a partial
    // socket failure must not return its budget. Only whole-batch success does.
    std::pmr::string output;
    const auto taken = connection.takeOutputBatch(16 * 1024, output, [](void* context, std::uint32_t streamId, std::size_t bytes) noexcept { static_cast<Http2DataOutputBudget*>(context)->noteDataOutput(streamId, bytes); }, &budget);
    RUVIA_CHECK(taken.status == ruvia::Http2OutputBatchStatus::kTaken);
    const auto frame = ruvia::parseHttp2FrameHeader(
        std::span<const char>(output.data(), output.size()));
    RUVIA_CHECK(frame.has_value());
    RUVIA_CHECK(frame && frame->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kData));
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        budget.reconcile(connection, false);
        co_return; }, stopIoOnCompletion(io));
    (void)io.poll();
    io.restart();
    RUVIA_CHECK(!secondAcquired);
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        budget.reconcile(connection, true);
        co_return; }, stopIoOnCompletion(io));
    io.run();
    io.restart();
    RUVIA_CHECK(secondAcquired);
    RUVIA_CHECK(connection.submitData(second, "x", ruvia::Http2EndStream::kKeepOpen) ==
                ruvia::Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK_EQ(connection.pendingDataOutputBytes(second), std::size_t{1});
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
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
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

    asio::post(io, [&io, signal, &termination] {
        signal->wake();
        (void)termination.terminate(std::make_error_code(std::errc::connection_aborted));
        io.stop();
    });
    io.run();
    io.restart();
    RUVIA_CHECK(signal->terminated());
    RUVIA_CHECK(table.remove(1));
    RUVIA_CHECK_EQ(table.dispatchedCount(), std::size_t{0});
    RUVIA_CHECK_EQ(table.size(), std::size_t{0});
}

RUVIA_TEST(http2_web_stream_signal_wakes_concurrent_waiters_without_self_cancel) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
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
        co_await ruvia::asAwaitable(signal->wait());
        ++wakeCount;
    };
    auto remaining = std::make_shared<std::size_t>(2);
    const auto stopWhenBothComplete = [&io, remaining](std::exception_ptr) {
        if (--*remaining == 0) {
            asio::post(io, [&io] { io.stop(); });
        }
    };
    asio::co_spawn(io, waitOnce(), stopWhenBothComplete);
    asio::co_spawn(io, waitOnce(), stopWhenBothComplete);
    (void)io.poll();
    RUVIA_CHECK_EQ(wakeCount, std::size_t{0});

    io.restart();
    asio::post(io, [signal] { signal->wake(); });
    io.run();
    io.restart();
    RUVIA_CHECK_EQ(wakeCount, std::size_t{2});
}

RUVIA_TEST(http2_web_stream_runtime_keeps_overflow_signal_reference_stable) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
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
