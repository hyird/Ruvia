#include <array>
#include <chrono>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/http/Hpack.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/Http2Framing.h"
#include "ruvia/web/detail/http2/Http2DataOutputBudget.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/web/detail/http2/Http2SansIoWsTransport.h"

#include "test_harness.h"
#include "test_io_context.h"

RUVIA_TEST(http2_websocket_transport_abort_wakes_budget_waiter) {
    std::pmr::monotonic_buffer_resource resource;
    auto connection = ruvia::Http2Connection::server({.resource = &resource});
    RUVIA_CHECK(connection.feed(ruvia::kHttp2ClientPreface) ==
                ruvia::Http2FeedResult::kAccepted);
    std::array<char, ruvia::kHttp2FrameHeaderBytes> handshakeSettings{};
    RUVIA_CHECK(ruvia::encodeHttp2FrameHeader(handshakeSettings, 0,
        ruvia::Http2FrameType::kSettings, 0, 0));
    RUVIA_CHECK(connection.feed(std::string_view(handshakeSettings.data(), handshakeSettings.size())) ==
                ruvia::Http2FeedResult::kAccepted);
    (void)connection.consumeOutput(connection.pendingOutput().size());
    std::pmr::string requestBlock(&resource);
    ruvia::HpackEncoder::encodeHeader(requestBlock, ":method", "GET");
    ruvia::HpackEncoder::encodeHeader(requestBlock, ":scheme", "https");
    ruvia::HpackEncoder::encodeHeader(requestBlock, ":path", "/");
    ruvia::HpackEncoder::encodeHeader(requestBlock, ":authority", "example.com");
    std::array<char, ruvia::kHttp2FrameHeaderBytes> requestHeader{};
    RUVIA_CHECK(ruvia::encodeHttp2FrameHeader(requestHeader,
        static_cast<std::uint32_t>(requestBlock.size()), ruvia::Http2FrameType::kHeaders,
        0x5, 1));  // END_HEADERS | END_STREAM
    std::pmr::string requestFrame(&resource);
    requestFrame.append(requestHeader.data(), requestHeader.size());
    requestFrame.append(requestBlock);
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

    std::array<char, ruvia::kHttp2FrameHeaderBytes + 6> peerSettings{};
    RUVIA_CHECK(ruvia::encodeHttp2FrameHeader(peerSettings, 6,
        ruvia::Http2FrameType::kSettings, 0, 0));
    peerSettings[9] = 0;
    peerSettings[10] = 4;
    RUVIA_CHECK(connection.feed(std::string_view(peerSettings.data(), peerSettings.size())) ==
                ruvia::Http2FeedResult::kAccepted);
    (void)connection.consumeOutput(connection.pendingOutput().size());
    ruvia::HttpResponse response({.resource = &resource});
    RUVIA_CHECK(connection.submitStreamingResponseHead(1, std::move(response)) ==
                ruvia::Http2SubmitStatus::kAccepted);
    (void)connection.consumeOutput(connection.pendingOutput().size());

    asio::io_context& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    ruvia::WorkerSignal writeSignal(worker);
    ruvia::detail::Http2DataOutputBudget budget(worker);
    ruvia::detail::Http2SansIoTermination termination;
    ruvia::detail::Http2SansIoStreamSignal streamSignal(worker, termination);
    ruvia::detail::Http2SansIoBodyQueue queue(&resource);
    ruvia::detail::Http2SansIoWsTransport<asio::any_io_executor> transport(
        connection, 1, queue, streamSignal, writeSignal, budget,
        asio::any_io_executor(io.get_executor()));

    bool completed = false;
    std::error_code writeError;
    asio::steady_timer watchdog(io);
    watchdog.expires_after(std::chrono::seconds(1));
    watchdog.async_wait([&](const std::error_code& error) {
        if (!error) {
            io.stop();
        }
    });
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            writeError = co_await ruvia::asAwaitable(transport.writeBytes(
                "blocked", ruvia::WebSocketServerTransportDisposition::kKeepOpen));
            completed = true;
            watchdog.cancel();
            attachment.stop(); }, asio::detached);
    const auto postResult = worker.post([&] { transport.abort(); });
    RUVIA_CHECK(postResult.accepted());
    io.run();
    RUVIA_CHECK(completed);
    RUVIA_CHECK(writeError == std::make_error_code(std::errc::operation_canceled));
    attachment.stop();
}
