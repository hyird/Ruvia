#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/WebSocketConnection.h"
#include "ruvia/web/WebSocketClient.h"

#include "test_harness.h"
#include "test_io_context.h"

namespace {
void exerciseClient(ruvia::testing::TestContext& ruvia_ctx, bool resetPeer) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peerFailure;
    bool sawExtendedConnect = false;
    bool sawClientEnd = false;
    unsigned messages = 0;
    const std::string large(2200000, 'a');
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::pmr::unsynchronized_pool_resource resource;
        auto connection = ruvia::Http2Connection::server({.resource = &resource});
        std::optional<ruvia::WebSocketConnection> websocket;
        std::optional<ruvia::Http2RequestHeadEvent> requestLease;
        std::string outbound;
        bool finish = false;
        bool ended = false;
        std::array<char, 16384> bytes{};
        std::pmr::string wire(&resource);
        for (;;) {
            while (!outbound.empty()) {
                const auto count = std::min<std::size_t>(outbound.size(), 16384);
                const auto result = connection.submitData(1, std::string_view(outbound.data(), count), ruvia::Http2EndStream::kKeepOpen);
                if (result == ruvia::Http2DataSubmitStatus::kBackpressured) {
                    break;
                }
                RUVIA_CHECK(result == ruvia::Http2DataSubmitStatus::kAccepted || result == ruvia::Http2DataSubmitStatus::kQueued);
                outbound.erase(0, count);
                if (result == ruvia::Http2DataSubmitStatus::kQueued) {
                    break;
                }
            }
            if (finish && outbound.empty() && !ended) {
                const auto result = connection.submitData(1, {}, ruvia::Http2EndStream::kEndStream);
                RUVIA_CHECK(result == ruvia::Http2DataSubmitStatus::kAccepted || result == ruvia::Http2DataSubmitStatus::kQueued);
                ended = true;
            }
            if (connection.wantsWrite()) {
                wire.clear();
                connection.takeOutput(wire);
                co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
            }
            if (sawClientEnd || (resetPeer && websocket)) {
                co_return;
            }
            const auto count = co_await socket.async_read_some(asio::buffer(bytes), asio::use_awaitable);
            const auto input = std::string_view(bytes.data(), count);
            for (;;) {
                const auto fed = connection.feed(input);
                RUVIA_CHECK(fed != ruvia::Http2FeedResult::kProtocolFailure);
                while (auto event = connection.nextEvent()) {
                    if (auto* request = event->requestHead()) {
                        const auto route = connection.serverRequestRoute(request->streamId());
                        sawExtendedConnect = route && route->webSocketConnect;
                        const auto validation = ruvia::validateHttp2WebSocketHandshake(connection, request->streamId(), request->request());
                        const auto submitted = connection.submitWebSocketHandshake(request->streamId(), request->request(), validation);
                        if (!submitted.submitted()) {
                            throw std::runtime_error("HTTP/2 WebSocket handshake failed");
                        }
                        websocket.emplace(ruvia::WebSocketConnectionOptions{.resource = &resource,
                            .compression = submitted.submitted()->compression()});
                        if (resetPeer) {
                            RUVIA_CHECK(connection.submitReset(1, ruvia::Http2ErrorCode::kCancel) == ruvia::Http2SubmitStatus::kAccepted);
                        }
                        requestLease.emplace(std::move(*request));
                    } else if (auto* data = event->tunnelData()) {
                        (void)websocket->feed(data->bytes());
                        (void)connection.acknowledge(data->takeCredit());
                        while (auto wsEvent = websocket->nextEvent()) {
                            if (auto* message = wsEvent->message()) {
                                ++messages;
                                RUVIA_CHECK_EQ(message->payload(), std::string_view(large));
                                RUVIA_CHECK(websocket->submitFrame(ruvia::WebSocketOpcode::kBinary, message->payload()) == ruvia::WebSocketFrameSubmitStatus::kAccepted);
                            } else if (wsEvent->close()) {
                                finish = true;
                            } else if (wsEvent->protocolError()) {
                                throw std::runtime_error("invalid tunneled WebSocket frame");
                            }
                            const auto output = websocket->outputPlan();
                            outbound.append(output.bytes());
                            (void)websocket->consumeOutput(output.bytes().size());
                            if (finish) {
                                break;
                            }
                        }
                    } else if (event->tunnelEnd()) {
                        sawClientEnd = true;
                    }
                }
                if (fed != ruvia::Http2FeedResult::kEventsPending) {
                    break;
                }
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peerFailure = failure; });
    bool resetObserved = false;
    const auto run = [&]() -> ruvia::Task<void> {
        ruvia::WebSocketClient client(attachment.loop(), {.scheme = ruvia::WebSocketScheme::kWs,
                                                             .protocol = ruvia::WebSocketClientProtocol::kHttp2,
                                                             .host = "127.0.0.1",
                                                             .port = peer.local_endpoint().port(),
                                                             .deflate = {.enabled = true},
                                                             .readTimeout = std::chrono::milliseconds(2000),
                                                             .writeTimeout = std::chrono::milliseconds(2000)});
        std::exception_ptr failure;
        try {
            co_await client.connect();
            if (resetPeer) {
                (void)co_await client.read();
            } else {
                for (const bool compress : {false, true}) {
                    co_await client.binary(large, {.compress = compress});
                    const auto echo = co_await client.read();
                    RUVIA_CHECK(echo.has_value());
                    if (echo) {
                        RUVIA_CHECK_EQ(echo->payload(), std::string_view(large));
                    }
                }
                co_await client.close({});
            }
        } catch (const ruvia::WebSocketClientError& error) {
            resetObserved = error.code() == ruvia::WebSocketClientError::Code::kProtocolError;
            if (!resetPeer || !resetObserved) {
                failure = std::current_exception();
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (peerFailure) {
        std::rethrow_exception(peerFailure);
    }
    RUVIA_CHECK(sawExtendedConnect);
    if (resetPeer) {
        RUVIA_CHECK(resetObserved);
    } else {
        RUVIA_CHECK_EQ(messages, 2U);
        RUVIA_CHECK(sawClientEnd);
    }
}
}  // namespace

RUVIA_TEST(http2_websocket_client_drives_flow_control_deflate_and_end_stream) {
    exerciseClient(ruvia_ctx, false);
}

RUVIA_TEST(http2_websocket_client_observes_peer_reset_and_joins_drivers) {
    exerciseClient(ruvia_ctx, true);
}
