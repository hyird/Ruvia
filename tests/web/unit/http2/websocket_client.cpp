#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/Http2Framing.h"
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
                        const auto requestView = connection.server_request_view(request->streamId());
                        sawExtendedConnect = requestView && requestView->method == "CONNECT" &&
                                             requestView->protocol == "websocket";
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

enum class closing_peer { clean_release,
    no_close,
    cancel_after_close,
    error_after_close,
    end_without_close };

void exercise_closing_peer(ruvia::testing::TestContext& ruvia_ctx, closing_peer behavior) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool received_close = false;
    bool released_request = false;
    std::vector<ruvia::Http2FrameType> terminal_frames;
    bool received_end_stream = false;
    std::optional<ruvia::Http2ErrorCode> reset_error;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        auto connection = ruvia::Http2Connection::server();
        std::optional<ruvia::Http2RequestHeadEvent> request;
        std::optional<ruvia::WebSocketConnection> websocket;
        std::array<char, 16384> input{};
        std::pmr::string output;
        for (;;) {
            if (connection.wantsWrite()) {
                output.clear();
                connection.takeOutput(output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            const auto count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            const auto bytes = std::string_view(input.data(), count);
            for (;;) {
                const auto fed = connection.feed(bytes);
                RUVIA_CHECK(fed != ruvia::Http2FeedResult::kProtocolFailure);
                while (auto event = connection.nextEvent()) {
                    if (auto* head = event->requestHead()) {
                        const auto validation = ruvia::validateHttp2WebSocketHandshake(connection, head->streamId(), head->request());
                        const auto submitted = connection.submitWebSocketHandshake(head->streamId(), head->request(), validation);
                        RUVIA_CHECK(submitted.submitted() != nullptr);
                        request.emplace(std::move(*head));
                        websocket.emplace();
                    } else if (auto* data = event->tunnelData()) {
                        (void)websocket->feed(data->bytes());
                        (void)connection.acknowledge(data->takeCredit());
                        while (auto frame = websocket->nextEvent()) {
                            if (const auto* close = frame->close()) {
                                received_close = true;
                                RUVIA_CHECK_EQ(close->closeCode(), std::uint16_t{1000});
                                RUVIA_CHECK_EQ(close->reason(), std::string_view("complete"));
                                break;
                            }
                            RUVIA_CHECK(frame->protocolError() == nullptr);
                        }
                    }
                }
                if (fed != ruvia::Http2FeedResult::kEventsPending) {
                    break;
                }
            }
            if (!received_close) {
                continue;
            }
            if (behavior != closing_peer::no_close && behavior != closing_peer::end_without_close) {
                const auto close = websocket->outputPlan();
                RUVIA_CHECK(!close.bytes().empty());
                RUVIA_CHECK(connection.submitData(1, close.bytes(), ruvia::Http2EndStream::kKeepOpen) == ruvia::Http2DataSubmitStatus::kAccepted);
                (void)websocket->consumeOutput(close.bytes().size());
            }
            if (behavior == closing_peer::clean_release || behavior == closing_peer::end_without_close) {
                RUVIA_CHECK(connection.submitData(1, {}, ruvia::Http2EndStream::kEndStream) == ruvia::Http2DataSubmitStatus::kAccepted);
            }
            // Publish the DATA/END_STREAM before releasing the real server lease.
            // Its half-open retirement emits NO_ERROR, in the same socket write.
            output.clear();
            connection.takeOutput(output);
            if (behavior == closing_peer::clean_release || behavior == closing_peer::no_close) {
                if (behavior == closing_peer::no_close) {
                    RUVIA_CHECK(connection.submitReset(1, ruvia::Http2ErrorCode::kNoError) == ruvia::Http2SubmitStatus::kAccepted);
                }
                RUVIA_CHECK(connection.release(std::move(*request)) == ruvia::Http2ServerRequestReleaseStatus::kReleased);
                released_request = true;
            } else if (behavior != closing_peer::end_without_close) {
                const auto error = behavior == closing_peer::cancel_after_close ? ruvia::Http2ErrorCode::kCancel : ruvia::Http2ErrorCode::kProtocolError;
                RUVIA_CHECK(connection.submitReset(1, error) == ruvia::Http2SubmitStatus::kAccepted);
            }
            std::pmr::string retirement;
            connection.takeOutput(retirement);
            output.append(retirement);
            for (std::size_t offset = 0; offset < output.size();) {
                const auto header = ruvia::parseHttp2FrameHeader(std::span<const char>(output.data() + offset, ruvia::kHttp2FrameHeaderBytes));
                RUVIA_CHECK(header.has_value());
                if (header->streamId == 1) {
                    terminal_frames.push_back(static_cast<ruvia::Http2FrameType>(header->type));
                    if (header->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kData) && (header->flags & 0x1U) != 0) {
                        received_end_stream = true;
                    }
                    if (header->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kRstStream)) {
                        RUVIA_CHECK_EQ(header->length, std::uint32_t{4});
                        const auto* code = output.data() + offset + ruvia::kHttp2FrameHeaderBytes;
                        const auto error = (std::uint32_t(static_cast<unsigned char>(code[0])) << 24) |
                                           (std::uint32_t(static_cast<unsigned char>(code[1])) << 16) |
                                           (std::uint32_t(static_cast<unsigned char>(code[2])) << 8) |
                                           std::uint32_t(static_cast<unsigned char>(code[3]));
                        reset_error = static_cast<ruvia::Http2ErrorCode>(error);
                    }
                }
                offset += ruvia::kHttp2FrameHeaderBytes + header->length;
            }
            co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            co_return;
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool protocol_error = false;
    bool close_completed = false;
    const auto run = [&]() -> ruvia::Task<void> {
        ruvia::WebSocketClient client(attachment.loop(), {.scheme = ruvia::WebSocketScheme::kWs,
                                                             .protocol = ruvia::WebSocketClientProtocol::kHttp2,
                                                             .host = "127.0.0.1",
                                                             .port = peer.local_endpoint().port()});
        std::exception_ptr failure;
        try {
            co_await client.connect();
            co_await client.close({.code = 1000, .reason = "complete"});
            close_completed = true;
        } catch (const ruvia::WebSocketClientError& error) {
            protocol_error = error.code() == ruvia::WebSocketClientError::Code::kProtocolError;
            if (!protocol_error) {
                failure = std::current_exception();
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    RUVIA_CHECK(received_close);
    RUVIA_CHECK(close_completed == (behavior == closing_peer::clean_release));
    RUVIA_CHECK(protocol_error == (behavior != closing_peer::clean_release));
    if (behavior == closing_peer::clean_release) {
        RUVIA_CHECK(released_request);
        RUVIA_CHECK_EQ(terminal_frames.size(), std::size_t{3});
        RUVIA_CHECK(terminal_frames[0] == ruvia::Http2FrameType::kData);
        RUVIA_CHECK(terminal_frames[1] == ruvia::Http2FrameType::kData);
        RUVIA_CHECK(terminal_frames[2] == ruvia::Http2FrameType::kRstStream);
        RUVIA_CHECK(received_end_stream);
        RUVIA_CHECK(reset_error == ruvia::Http2ErrorCode::kNoError);
    }
    if (behavior == closing_peer::cancel_after_close) {
        RUVIA_CHECK(reset_error == ruvia::Http2ErrorCode::kCancel);
    } else if (behavior == closing_peer::error_after_close) {
        RUVIA_CHECK(reset_error == ruvia::Http2ErrorCode::kProtocolError);
    } else if (behavior == closing_peer::no_close) {
        RUVIA_CHECK(reset_error == ruvia::Http2ErrorCode::kNoError);
        RUVIA_CHECK(!received_end_stream);
    }
}
}  // namespace

RUVIA_TEST(http2_websocket_client_drives_flow_control_deflate_and_end_stream) {
    exerciseClient(ruvia_ctx, false);
}

RUVIA_TEST(http2_websocket_client_observes_peer_reset_and_joins_drivers) {
    exerciseClient(ruvia_ctx, true);
}

RUVIA_TEST(http2_websocket_normal_close_consumes_peer_close_before_clean_stream_retirement) {
    exercise_closing_peer(ruvia_ctx, closing_peer::clean_release);
}

RUVIA_TEST(http2_websocket_no_error_reset_without_peer_close_is_not_a_close_handshake) {
    exercise_closing_peer(ruvia_ctx, closing_peer::no_close);
}

RUVIA_TEST(http2_websocket_cancel_reset_after_buffered_close_remains_an_error) {
    exercise_closing_peer(ruvia_ctx, closing_peer::cancel_after_close);
}

RUVIA_TEST(http2_websocket_protocol_reset_after_buffered_close_remains_an_error) {
    exercise_closing_peer(ruvia_ctx, closing_peer::error_after_close);
}

RUVIA_TEST(http2_websocket_end_stream_without_peer_close_is_not_a_close_handshake) {
    exercise_closing_peer(ruvia_ctx, closing_peer::end_without_close);
}
