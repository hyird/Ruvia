#include "test_harness.h"

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <filesystem>
#include <fstream>

#include "ruvia/http/detail/HttpResponseFileAccess.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/web/detail/http/ContextServices.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2Hpack.h"
#include "ruvia/http/detail/http2/Http2RequestBuilder.h"
#include "ruvia/http/detail/http2/Http2WindowUpdate.h"
#include "ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h"
#include "ruvia/web/detail/server/Http2SansIoSession.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/detail/router/RouterInternal.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/detail/SansIoDriver.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/Router.h"

namespace {

using asio::ip::tcp;
using ruvia::detail::Http2Connection;
using ruvia::detail::Http2DataSubmitStatus;
using ruvia::detail::Http2EndStream;
using ruvia::detail::Http2FrameType;
using ruvia::detail::HpackEncoder;

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// A real route handler: returns a distinctive body so the client can confirm the
// registered handler actually ran through the sans-I/O dispatch pipeline.
ruvia::Task<ruvia::HttpResponse> echoHandler(void*, ruvia::Context& ctx) {
    co_return ctx.text("handler-ran");
}

// A slow handler: suspends on a timer (executor passed via the handler context) before
// responding, so a concurrently-dispatched fast handler can finish first.
ruvia::Task<ruvia::HttpResponse> slowHandler(void* context, ruvia::Context& ctx) {
    auto* io = static_cast<asio::io_context*>(context);
    asio::steady_timer timer(*io);
    timer.expires_after(std::chrono::milliseconds(30));
    const auto waitEc = co_await ruvia::detail::asyncError([&timer](auto handler) mutable {
        timer.async_wait(std::move(handler));
    });
    (void)waitEc;
    co_return ctx.text("slow");
}

ruvia::Task<ruvia::HttpResponse> fastHandler(void*, ruvia::Context& ctx) {
    co_return ctx.text("fast");
}

// Streaming request-body handler: drains the body reader, records the total bytes seen
// via the void* handler context, and replies with a fixed marker.
// Returns a large BUFFERED body (100 KiB) to exercise the buffered-response send-window
// pacing path (distinct from the file-body path).
constexpr std::size_t kLargeBufferedBytes = 100000;
ruvia::Task<ruvia::HttpResponse> largeBufferedHandler(void*, ruvia::Context&) {
    ruvia::HttpResponse response(std::pmr::get_default_resource());
    response.status(200);
    std::string body(kLargeBufferedBytes, 'Q');
    response.setBodyCopy(body);
    co_return response;
}

ruvia::Task<ruvia::HttpResponse> streamBodyCountHandler(void* ctx, ruvia::Context& c) {
    auto* out = static_cast<std::size_t*>(ctx);
    std::size_t bytes = 0;
    auto& reader = c.req().bodyReader();
    while (auto chunk = co_await reader.read()) {
        bytes += chunk->size();
    }
    *out = bytes;
    co_return c.text("upload-done");
}

// Path + size of the large temp file the pacing test serves (set in the test body).
inline std::string& largeFilePath() {
    static std::string path;
    return path;
}
constexpr std::uint64_t kLargeFileBytes = 200000;  // > default send window (65535)

// A plain (buffered) route returning a FILE body larger than the send window: this is
// the path that had NO stream signal, so a window block could never be woken.
ruvia::Task<ruvia::HttpResponse> largeFileHandler(void*, ruvia::Context&) {
    ruvia::HttpResponse response(std::pmr::get_default_resource());
    response.status(200);
    ruvia::detail::setResponseFileBody(
        response, std::filesystem::path(largeFilePath()), kLargeFileBytes, 0, kLargeFileBytes);
    co_return response;
}

// A WebSocket echo handler: echoes each text message back and finishes when the peer
// closes (read returns nullopt).
ruvia::Task<void> wsEchoHandler(void*, ruvia::Context& ctx) {
    auto& ws = ctx.webSocket();
    while (auto message = co_await ws.read()) {
        if (message->text()) {
            co_await ws.text(message->payload());
        }
    }
}

// Returns without waiting for peer input so runWebSocketSession initiates the
// server side of the closing handshake.
ruvia::Task<void> wsServerCloseHandler(void*, ruvia::Context&) {
    co_return;
}

// Build a masked client->server WebSocket frame (RFC 6455 §5.1, short lengths only).
std::string maskedWsFrame(std::uint8_t opcode, std::string_view payload, bool rsv1 = false) {
    std::string f;
    f.push_back(static_cast<char>(0x80U | (rsv1 ? 0x40U : 0U) | opcode));  // FIN | RSV1? | opcode
    f.push_back(static_cast<char>(0x80U | static_cast<std::uint8_t>(payload.size())));
    const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
    f.append(reinterpret_cast<const char*>(mask), 4);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]));
    }
    return f;
}

std::string frame(std::uint8_t type, std::uint8_t flags, std::uint32_t streamId, std::string_view payload) {
    std::string bytes(ruvia::detail::kHttp2FrameHeaderBytes, '\0');
    ruvia::detail::http2WriteFrameHeader(
        bytes.data(), static_cast<std::uint32_t>(payload.size()),
        static_cast<Http2FrameType>(type), flags, streamId);
    bytes.append(payload);
    return bytes;
}

}  // namespace

// End-to-end proof that the generic sans-I/O driver (ruvia-core) can back a real
// HTTP/2 server over a real socket using ONLY the Http2Connection core: a synthetic
// client sends a GET; the pump feeds the core, the onReadable callback dispatches a
// 200 "pong" response, and the pump flushes it back. Validates the driver contract and
// the core's external usability with zero coroutine sessions.
RUVIA_TEST(sansio_driver_h2_get_round_trip) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotPong = false;

    // Server: drive Http2Connection with the generic pump. onReadable answers each
    // completed request with a fixed 200 "pong".
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            std::pmr::monotonic_buffer_resource resource;
            Http2Connection conn(&resource);
            conn.beginConnection();

            auto onReadable = [&resource, &ruvia_ctx](Http2Connection& c) -> ruvia::Task<void> {
                for (;;) {
                    const auto event = c.nextEvent();
                    if (!event.has_value()) {
                        break;
                    }
                    if (const auto* messageEnd = event->messageEnd()) {
                        const auto streamId = messageEnd->streamId();
                        ruvia::HttpResponse response(&resource);
                        response.status(200);
                        response.setBodyCopy("pong");
                        RUVIA_CHECK(
                            c.submitResponseHead(streamId, response).submitted() !=
                            nullptr);
                        RUVIA_CHECK(c.submitData(
                            streamId, "pong", Http2EndStream::kEndStream) ==
                            Http2DataSubmitStatus::kAccepted);
                    }
                }
                co_return;
            };
            co_await ruvia::detail::taskAsAwaitable(
                ruvia::detail::pumpSansIoConnection(
                    conn,
                    sock,
                    [](const Http2Connection& connection) noexcept {
                        return connection.connectionError().has_value();
                    },
                    onReadable));
        },
        asio::detached);

    // Client: a synthetic HTTP/2 peer that GETs / and looks for the DATA reply.
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(
                    0x1 /*HEADERS*/,
                    ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders,
                    1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            // Drain frames until the stream-1 DATA reply arrives.
            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    header.streamId == 1) {
                    gotPong = (std::string_view(payload) == "pong");
                    break;
                }
            }

            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotPong);
}

// End-to-end proof that REAL framework dispatch runs over the sans-I/O core: onReadable
// builds an HttpRequest from the stream (Http2RequestBuilder), resolves it against a
// RouteTable, and runs the actual dispatchBuffered pipeline (which 404s an empty table),
// then submits the response. The client verifies a response HEADERS frame comes back --
// proving request-build -> resolve -> dispatch -> submit works with no coroutine session.
RUVIA_TEST(sansio_driver_h2_real_dispatch_round_trip) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotResponseHead = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::RouteTable routes(worker.resource());  // empty -> 404
            // Drive the packaged, reusable buffered session helper.
            co_await ruvia::detail::taskAsAwaitable(
                ruvia::detail::runHttp2SansIoSession(sock, routes, worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/missing");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(
                    0x1, ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders,
                    1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders) &&
                    header.streamId == 1) {
                    gotResponseHead = true;
                    break;
                }
            }

            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotResponseHead);
}

// End-to-end proof that a REAL registered handler runs over the sans-I/O core with a
// request body: a POST /echo route echoes the body; the buffered helper accumulates the
// DATA into the stream, dispatches to the handler, and submits the echoed response.
RUVIA_TEST(sansio_driver_h2_post_echo_real_handler) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string echoed;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(
                ruvia::HttpKnownMethod::kPost,
                std::pmr::string("/echo", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &echoHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "POST");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/echo");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            // HEADERS (END_HEADERS, body follows) then DATA "hello" (END_STREAM).
            if (!co_await writeAll(frame(
                    0x1, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }
            if (!co_await writeAll(frame(0x0 /*DATA*/, ruvia::detail::kHttp2FlagEndStream, 1, "hello"))) {
                co_return;
            }

            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    header.streamId == 1 && !payload.empty()) {
                    echoed = payload;
                    break;
                }
            }

            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(echoed == "handler-ran");
}

// Multiplexing proof: two concurrent requests -- stream 1 to a SLOW handler, stream 3
// to a FAST one -- must both complete, and the fast response must come back first even
// though its request arrived second. That out-of-order completion proves the handlers
// run concurrently rather than blocking the read/dispatch loop.
RUVIA_TEST(sansio_driver_h2_concurrent_streams_multiplex) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::vector<std::pair<std::uint32_t, std::string>> replies;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(
                ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/slow", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(&io, &slowHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.registerRoute(
                ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/fast", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &fastHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto requestOn = [](std::uint32_t streamId, std::string_view path) {
                std::pmr::string block(std::pmr::get_default_resource());
                HpackEncoder::encodeHeader(block, ":method", "GET");
                HpackEncoder::encodeHeader(block, ":path", path);
                HpackEncoder::encodeHeader(block, ":scheme", "http");
                HpackEncoder::encodeHeader(block, ":authority", "localhost");
                return frame(
                    0x1, ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders,
                    streamId, std::string_view(block.data(), block.size()));
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4, 0, 0, {}))) co_return;
            if (!co_await writeAll(requestOn(1, "/slow"))) co_return;  // slow first
            if (!co_await writeAll(requestOn(3, "/fast"))) co_return;  // fast second

            while (replies.size() < 2) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) && !payload.empty()) {
                    replies.emplace_back(header.streamId, payload);
                }
            }

            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(replies.size(), static_cast<std::size_t>(2));
    // The fast handler (stream 3, requested second) completes and replies first.
    RUVIA_CHECK_EQ(replies[0].first, static_cast<std::uint32_t>(3));
    RUVIA_CHECK(replies[0].second == "fast");
    RUVIA_CHECK_EQ(replies[1].first, static_cast<std::uint32_t>(1));
    RUVIA_CHECK(replies[1].second == "slow");
}

// End-to-end WebSocket over the sans-I/O session (RFC 8441 Extended CONNECT): the
// client opens a tunnel to a registered WebSocket echo route, sends a masked text
// frame as HTTP/2 DATA, and must get the unmasked echo back; a client Close is then
// answered with the server's Close carrying END_STREAM. Proves the per-stream inbound
// pipe + Http2SansIoWsTransport + the shared runWebSocketSession over the core.
RUVIA_TEST(sansio_driver_h2_websocket_echo) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotHandshake = false;
    std::string echoedFrame;      // reassembled ws frame bytes from stream-1 DATA
    bool gotCloseEndStream = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerStreamRoute(
                ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsEchoHandler),
                ruvia::detail::ResponseBodyMode::kWebSocket,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto readFrameInto = [&readExact](
                ruvia::detail::Http2FrameHeader& header, std::string& payload) -> asio::awaitable<bool> {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) co_return false;
                header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                payload.assign(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    co_return false;
                }
                co_return true;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "CONNECT");
            HpackEncoder::encodeHeader(headerBlock, ":protocol", "websocket");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/ws");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(headerBlock, "sec-websocket-version", "13");
            // Extended CONNECT: END_HEADERS only -- the stream MUST stay open.
            if (!co_await writeAll(frame(
                    0x1 /*HEADERS*/, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            // Wait for the 200 handshake HEADERS on stream 1.
            ruvia::detail::Http2FrameHeader header{};
            std::string payload;
            for (;;) {
                if (!co_await readFrameInto(header, payload)) co_return;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders) &&
                    header.streamId == 1) {
                    gotHandshake = true;
                    break;
                }
            }

            // Send a masked text frame through the tunnel and reassemble the echo
            // (the transport may split the ws frame across DATA frames).
            if (!co_await writeAll(frame(0x0 /*DATA*/, 0, 1, maskedWsFrame(0x1, "hello")))) co_return;
            std::string tunnelBytes;
            while (echoedFrame.empty()) {
                if (!co_await readFrameInto(header, payload)) co_return;
                if (header.type != static_cast<std::uint8_t>(Http2FrameType::kData) ||
                    header.streamId != 1) {
                    continue;
                }
                tunnelBytes += payload;
                if (tunnelBytes.size() >= 2) {
                    const auto len = static_cast<std::size_t>(
                        static_cast<unsigned char>(tunnelBytes[1]) & 0x7FU);
                    if (tunnelBytes.size() >= 2 + len) {
                        echoedFrame = tunnelBytes.substr(0, 2 + len);
                    }
                }
            }

            // Close the tunnel: the client sends its masked Close and orderly
            // transport half-close together; the server echoes Close and ends its
            // half only after the protocol core observes that peer Close.
            if (!co_await writeAll(frame(
                    0x0 /*DATA*/, ruvia::detail::kHttp2FlagEndStream, 1,
                    maskedWsFrame(0x8, std::string_view("\x03\xE8", 2))))) {
                co_return;
            }
            for (;;) {
                if (!co_await readFrameInto(header, payload)) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    header.streamId == 1 &&
                    (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                    gotCloseEndStream = true;
                    break;
                }
            }

            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotHandshake);
    // FIN|text, length 5, "hello" -- unmasked server frame, payload echoed intact.
    RUVIA_CHECK_EQ(echoedFrame.size(), static_cast<std::size_t>(7));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(echoedFrame[0]), static_cast<unsigned char>(0x81));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(echoedFrame[1]), static_cast<unsigned char>(5));
    RUVIA_CHECK(echoedFrame.substr(2) == "hello");
    RUVIA_CHECK(gotCloseEndStream);
}

// A server-initiated RFC 6455 Close is not itself RFC 8441 transport EOF. The first
// DATA carries only the Close frame and keeps the h2 send half open; after the client
// replies with Close+END_STREAM, the server emits its separate empty DATA+END_STREAM.
// This pins the typed WsOutputPlan -> Http2EndStream mapping and prevents a runtime
// from reconstructing END_STREAM from "we sent a Close" again.
RUVIA_TEST(sansio_driver_h2_server_close_waits_for_peer_close) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotHandshake = false;
    bool gotCloseWithoutEnd = false;
    bool gotTerminalEmptyEnd = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerStreamRoute(
                ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/server-close", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsServerCloseHandler),
                ruvia::detail::ResponseBodyMode::kWebSocket,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()),
                    asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto readFrameInto = [&readExact](
                ruvia::detail::Http2FrameHeader& header,
                std::string& payload) -> asio::awaitable<bool> {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                    co_return false;
                }
                header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                payload.assign(header.length, '\0');
                if (header.length != 0 &&
                    !co_await readExact(payload.data(), payload.size())) {
                    co_return false;
                }
                co_return true;
            };

            if (!co_await writeAll(kClientPreface) ||
                !co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "CONNECT");
            HpackEncoder::encodeHeader(headerBlock, ":protocol", "websocket");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/server-close");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(headerBlock, "sec-websocket-version", "13");
            if (!co_await writeAll(frame(
                    0x1 /*HEADERS*/, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            ruvia::detail::Http2FrameHeader header{};
            std::string payload;
            while (!gotCloseWithoutEnd) {
                if (!co_await readFrameInto(header, payload)) {
                    co_return;
                }
                if (header.streamId != 1) {
                    continue;
                }
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders)) {
                    gotHandshake = true;
                    continue;
                }
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    payload.size() >= 4 &&
                    static_cast<unsigned char>(payload[0]) == 0x88U) {
                    gotCloseWithoutEnd =
                        (header.flags & ruvia::detail::kHttp2FlagEndStream) == 0;
                }
            }

            if (!co_await writeAll(frame(
                    0x0 /*DATA*/, ruvia::detail::kHttp2FlagEndStream, 1,
                    maskedWsFrame(0x8, std::string_view("\x03\xE8", 2))))) {
                co_return;
            }
            for (;;) {
                if (!co_await readFrameInto(header, payload)) {
                    break;
                }
                if (header.streamId == 1 &&
                    header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                    gotTerminalEmptyEnd = payload.empty();
                    break;
                }
            }

            asio::error_code ignored;
            sock.shutdown(tcp::socket::shutdown_both, ignored);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotHandshake);
    RUVIA_CHECK(gotCloseWithoutEnd);
    RUVIA_CHECK(gotTerminalEmptyEnd);
}

// An Extended CONNECT to a WebSocket route with a bad sec-websocket-version must be
// answered with a buffered error response (HEADERS then DATA+END_STREAM), mirroring
// the coroutine session's invalid-handshake 400 path.
RUVIA_TEST(sansio_driver_h2_websocket_invalid_version_rejected) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotResponseHead = false;
    bool gotEndStream = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerStreamRoute(
                ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsEchoHandler),
                ruvia::detail::ResponseBodyMode::kWebSocket,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "CONNECT");
            HpackEncoder::encodeHeader(headerBlock, ":protocol", "websocket");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/ws");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(headerBlock, "sec-websocket-version", "12");  // bad
            if (!co_await writeAll(frame(
                    0x1, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.streamId != 1) continue;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders)) {
                    gotResponseHead = true;
                }
                if ((header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                    gotEndStream = true;
                    break;
                }
            }

            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotResponseHead);
    RUVIA_CHECK(gotEndStream);
}

namespace {

// Streaming handler that atomically ends with a trailer section: the h2 stream must
// end with trailing HEADERS (END_STREAM) instead of an empty DATA frame.
ruvia::Task<void> streamTrailerHandler(void*, ruvia::Context& c) {
    auto& stream = c.streamText();
    co_await stream.write("body-part");
    const std::array<ruvia::HttpHeaderView, 1> trailers{
        ruvia::HttpHeaderView{"x-checksum", "abc123"}};
    co_await stream.end(trailers);
}

// Streaming handler pushing one large chunk; used to exercise send-window pacing.
ruvia::Task<void> streamBigChunkHandler(void*, ruvia::Context& c) {
    auto& stream = c.streamText();
    co_await stream.write(std::string(64, 'z'));
}

struct HpackCollect {
    std::string joined;
    static bool onHeader(void* target, std::string_view name, std::string_view value) {
        auto* self = static_cast<HpackCollect*>(target);
        self->joined.append(name).append("=").append(value).append(";");
        return true;
    }
};

}  // namespace

// Expect is one cross-version semantic contract. Stream 1 sends a legal repeated/
// empty-member 100-continue list and withholds DATA until the server's exact interim
// head arrives. Stream 3 sends an unknown extension and withholds DATA permanently;
// the Web product must answer 417 immediately instead of the HTTP core rejecting the
// field block or the buffered dispatcher deadlocking while it waits for content.
RUVIA_TEST(sansio_driver_h2_expectation_decision_precedes_request_content) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotContinue = false;
    bool continueEndedStream = false;
    bool gotUnsupportedFinal = false;
    bool gotSupportedFinal = false;
    std::string supportedBody;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(
                ruvia::HttpKnownMethod::kPost,
                std::pmr::string("/echo", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &echoHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(
                ruvia::detail::runHttp2SansIoSession(
                    sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
                asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_write(
                    sock,
                    asio::buffer(bytes.data(), bytes.size()),
                    asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == bytes.size();
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_read(
                    sock,
                    asio::buffer(data, size),
                    asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface) ||
                !co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            const auto makeHead = [](std::string_view expect) {
                std::pmr::string block(std::pmr::get_default_resource());
                HpackEncoder::encodeHeader(block, ":method", "POST");
                HpackEncoder::encodeHeader(block, ":path", "/echo");
                HpackEncoder::encodeHeader(block, ":scheme", "http");
                HpackEncoder::encodeHeader(block, ":authority", "localhost");
                HpackEncoder::encodeHeader(block, "content-length", "5");
                HpackEncoder::encodeHeader(block, "expect", expect);
                return block;
            };
            const auto continueHead = makeHead(", 100-continue,");
            const auto unsupportedHead = makeHead("custom-feature");
            if (!co_await writeAll(frame(
                    0x1 /*HEADERS*/,
                    ruvia::detail::kHttp2FlagEndHeaders,
                    1,
                    std::string_view(continueHead.data(), continueHead.size()))) ||
                !co_await writeAll(frame(
                    0x1 /*HEADERS*/,
                    ruvia::detail::kHttp2FlagEndHeaders,
                    3,
                    std::string_view(unsupportedHead.data(), unsupportedHead.size())))) {
                co_return;
            }

            ruvia::detail::HpackDecoder decoder(std::pmr::get_default_resource());
            bool contentSent = false;
            bool supportedEnded = false;
            while (!(gotContinue && gotUnsupportedFinal && supportedEnded)) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                    break;
                }
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 &&
                    !co_await readExact(payload.data(), payload.size())) {
                    break;
                }

                if (header.type ==
                        static_cast<std::uint8_t>(Http2FrameType::kHeaders) &&
                    (header.streamId == 1 || header.streamId == 3)) {
                    HpackCollect fields;
                    const auto decoded = decoder.decode(
                        payload, &fields, &HpackCollect::onHeader);
                    RUVIA_CHECK(decoded.ok());
                    if (!decoded.ok()) {
                        break;
                    }
                    if (header.streamId == 1 &&
                        fields.joined.find(":status=100;") != std::string::npos) {
                        gotContinue = true;
                        continueEndedStream =
                            (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0;
                        RUVIA_CHECK(fields.joined == ":status=100;");
                        if (!contentSent) {
                            contentSent = co_await writeAll(frame(
                                0x0 /*DATA*/,
                                ruvia::detail::kHttp2FlagEndStream,
                                1,
                                "hello"));
                            if (!contentSent) {
                                break;
                            }
                        }
                    } else if (header.streamId == 1 &&
                               fields.joined.find(":status=200;") !=
                                   std::string::npos) {
                        gotSupportedFinal = true;
                    } else if (header.streamId == 3 &&
                               fields.joined.find(":status=417;") !=
                                   std::string::npos) {
                        gotUnsupportedFinal = true;
                    }
                } else if (header.type ==
                               static_cast<std::uint8_t>(Http2FrameType::kData) &&
                           header.streamId == 1 && gotSupportedFinal) {
                    supportedBody.append(payload);
                    supportedEnded =
                        (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0;
                }
            }

            asio::error_code ignored;
            sock.shutdown(tcp::socket::shutdown_both, ignored);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotContinue);
    RUVIA_CHECK(!continueEndedStream);
    RUVIA_CHECK(gotUnsupportedFinal);
    RUVIA_CHECK(gotSupportedFinal);
    RUVIA_CHECK(supportedBody == "handler-ran");
}

// Trailers over the sans-I/O h2 streaming path: HEAD(no END_STREAM), DATA body, then a
// trailing HEADERS frame carrying END_STREAM whose block decodes to the terminal section.
RUVIA_TEST(sansio_driver_h2_stream_trailers_emitted) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string body;
    std::string trailerFields;
    bool trailerEndStream = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerStreamRoute(
                ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/trail", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &streamTrailerHandler),
                ruvia::detail::ResponseBodyMode::kStream,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4, 0, 0, {}))) co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/trail");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(
                    0x1, ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders,
                    1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            ruvia::detail::HpackDecoder decoder(std::pmr::get_default_resource());
            bool sawHead = false;
            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.streamId != 1) continue;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders)) {
                    HpackCollect collect;
                    (void)decoder.decode(payload, &collect, &HpackCollect::onHeader);
                    if (!sawHead) {
                        sawHead = true;
                    } else {
                        trailerFields = collect.joined;  // the trailing HEADERS block
                        trailerEndStream =
                            (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0;
                        break;
                    }
                } else if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData)) {
                    body += payload;
                    if ((header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) break;
                }
            }
            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(body == "body-part");
    RUVIA_CHECK(trailerFields == "x-checksum=abc123;");
    RUVIA_CHECK(trailerEndStream);
}

// permessage-deflate over h2 Extended CONNECT: the handshake echoes the negotiated
// extension, and a compressed (RSV1) client frame is inflated before reaching the
// handler, whose echo round-trips intact.
RUVIA_TEST(sansio_driver_h2_websocket_permessage_deflate) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string handshakeFields;
    std::string echoedFrame;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerStreamRoute(
                ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsEchoHandler),
                ruvia::detail::ResponseBodyMode::kWebSocket,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4, 0, 0, {}))) co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "CONNECT");
            HpackEncoder::encodeHeader(headerBlock, ":protocol", "websocket");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/ws");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(headerBlock, "sec-websocket-version", "13");
            HpackEncoder::encodeHeader(
                headerBlock, "sec-websocket-extensions", "permessage-deflate; client_max_window_bits");
            if (!co_await writeAll(frame(
                    0x1, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            // Handshake HEADERS: decode and capture the echoed extension.
            ruvia::detail::HpackDecoder decoder(std::pmr::get_default_resource());
            ruvia::detail::Http2FrameHeader header{};
            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) co_return;
                header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) co_return;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders) &&
                    header.streamId == 1) {
                    HpackCollect collect;
                    (void)decoder.decode(payload, &collect, &HpackCollect::onHeader);
                    handshakeFields = collect.joined;
                    break;
                }
            }

            // Send a COMPRESSED masked text frame; the server must inflate + echo it.
            ruvia::detail::WebSocketDeflate clientCodec;
            std::pmr::string compressed(std::pmr::get_default_resource());
            if (!clientCodec.compress("hello-deflate", compressed)) co_return;
            if (!co_await writeAll(frame(
                    0x0, 0, 1,
                    maskedWsFrame(0x1, std::string_view(compressed.data(), compressed.size()),
                                  /*rsv1=*/true)))) {
                co_return;
            }
            std::string tunnelBytes;
            while (echoedFrame.empty()) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) co_return;
                header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) co_return;
                if (header.type != static_cast<std::uint8_t>(Http2FrameType::kData) ||
                    header.streamId != 1) {
                    continue;
                }
                tunnelBytes += payload;
                if (tunnelBytes.size() >= 2) {
                    const auto len = static_cast<std::size_t>(
                        static_cast<unsigned char>(tunnelBytes[1]) & 0x7FU);
                    if (tunnelBytes.size() >= 2 + len) {
                        echoedFrame = tunnelBytes.substr(0, 2 + len);
                    }
                }
            }
            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(handshakeFields.find("sec-websocket-extensions=permessage-deflate") != std::string::npos);
    // 13-byte echo does not shrink under deflate, so it comes back as a plain frame.
    RUVIA_CHECK_EQ(echoedFrame.size(), static_cast<std::size_t>(15));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(echoedFrame[0]), static_cast<unsigned char>(0x81));
    RUVIA_CHECK(echoedFrame.substr(2) == "hello-deflate");
}

// Send-window pacing: with a tiny stream window the streaming sink must park until the
// client grants WINDOW_UPDATEs, and every byte must still arrive, ending the stream.
RUVIA_TEST(sansio_driver_h2_stream_send_window_pacing) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::size_t received = 0;
    bool sawEnd = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerStreamRoute(
                ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/big", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &streamBigChunkHandler),
                ruvia::detail::ResponseBodyMode::kStream,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            // SETTINGS_INITIAL_WINDOW_SIZE = 8: the 64-byte body must be granted 8 at a time.
            const char settingsPayload[6] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x08};
            if (!co_await writeAll(frame(0x4, 0, 0, std::string_view(settingsPayload, 6)))) co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/big");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(
                    0x1, ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders,
                    1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.streamId != 1) continue;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData)) {
                    received += payload.size();
                    if ((header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                        sawEnd = true;
                        break;
                    }
                    if (!payload.empty()) {
                        // Grant the next window slice (connection + stream scoped).
                        char updates[2 * (ruvia::detail::kHttp2FrameHeaderBytes + 4)];
                        char* out = ruvia::detail::http2WriteWindowUpdate(
                            updates, 0, static_cast<std::uint32_t>(payload.size()));
                        out = ruvia::detail::http2WriteWindowUpdate(
                            out, 1, static_cast<std::uint32_t>(payload.size()));
                        if (!co_await writeAll(std::string_view(
                                updates, static_cast<std::size_t>(out - updates)))) {
                            break;
                        }
                    }
                }
            }
            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(received, static_cast<std::size_t>(64));
    RUVIA_CHECK(sawEnd);
}

// P0 regression: a plain route returning a FILE body larger than the send window must
// pace on WINDOW_UPDATEs and deliver EVERY byte + END_STREAM. Before the fix, such
// streams got no Http2SansIoStreamSignal, so the first window-blocked file chunk could
// never be woken -- the response was silently truncated and the stream hung.
RUVIA_TEST(sansio_driver_h2_large_file_body_paces_and_completes) {
    // Write the temp file (kLargeFileBytes of a repeating pattern).
    const auto path = (std::filesystem::temp_directory_path() /
                       "ruvia_sansio_large_file_test.bin").string();
    largeFilePath() = path;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        std::string block(4096, '\0');
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = static_cast<char>('A' + (i % 26));
        }
        std::uint64_t written = 0;
        while (written < kLargeFileBytes) {
            const auto n = static_cast<std::size_t>(
                std::min<std::uint64_t>(block.size(), kLargeFileBytes - written));
            out.write(block.data(), static_cast<std::streamsize>(n));
            written += n;
        }
    }

    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::uint64_t received = 0;
    bool sawEnd = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(
                ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/file", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &largeFileHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            // Small initial window (16) so the file body blocks almost immediately and
            // only completes if WINDOW_UPDATE-driven pacing wakes it repeatedly.
            const char settingsPayload[6] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x10};
            if (!co_await writeAll(frame(0x4, 0, 0, std::string_view(settingsPayload, 6)))) co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/file");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(
                    0x1, ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders,
                    1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.streamId != 1 ||
                    header.type != static_cast<std::uint8_t>(Http2FrameType::kData)) {
                    continue;
                }
                received += payload.size();
                if ((header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                    sawEnd = true;
                    break;
                }
                if (!payload.empty()) {
                    char updates[2 * (ruvia::detail::kHttp2FrameHeaderBytes + 4)];
                    char* out = ruvia::detail::http2WriteWindowUpdate(
                        updates, 0, static_cast<std::uint32_t>(payload.size()));
                    out = ruvia::detail::http2WriteWindowUpdate(
                        out, 1, static_cast<std::uint32_t>(payload.size()));
                    if (!co_await writeAll(std::string_view(
                            updates, static_cast<std::size_t>(out - updates)))) {
                        break;
                    }
                }
            }
            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(received, kLargeFileBytes);  // every byte delivered, not truncated
    RUVIA_CHECK(sawEnd);
}

// P2 coverage: a streaming request body (RequestBodyMode::kStream) flows through the
// live session to the handler's body reader chunk by chunk. Guards the signal-wake /
// body-queue handoff -- a regression there would hang a streaming upload forever.
RUVIA_TEST(sansio_driver_h2_streaming_request_body) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::size_t receivedBytes = 0;
    std::string body;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(
                ruvia::HttpKnownMethod::kPost,
                std::pmr::string("/upload", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(&receivedBytes, &streamBodyCountHandler),
                ruvia::detail::RequestBodyMode::kStream,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto yield = [&io]() -> asio::awaitable<void> {
                asio::steady_timer t(io, std::chrono::milliseconds(5));
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4, 0, 0, {}))) co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "POST");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/upload");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            // HEADERS with NO END_STREAM -> body follows in separate DATA frames.
            if (!co_await writeAll(frame(
                    0x1, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }
            co_await yield();
            if (!co_await writeAll(frame(0x0, 0, 1, "aaa"))) co_return;  // 3 bytes
            co_await yield();
            if (!co_await writeAll(frame(0x0, 0, 1, "bb"))) co_return;   // 2 bytes
            co_await yield();
            if (!co_await writeAll(frame(0x0, ruvia::detail::kHttp2FlagEndStream, 1, {}))) co_return;

            for (;;) {
                char hb[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(hb, sizeof(hb))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(hb, sizeof(hb)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    header.streamId == 1 && !payload.empty()) {
                    body = payload;
                    break;
                }
            }
            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(receivedBytes, static_cast<std::size_t>(5));  // "aaa" + "bb"
    RUVIA_CHECK(body == "upload-done");
}

// P2 coverage: a server-role request framed with trailers (DATA then a trailing
// HEADERS with END_STREAM, gRPC-style) must dispatch normally. Guards the
// processTrailerHeaders server path (client-role trailers were the only coverage).
RUVIA_TEST(sansio_driver_h2_server_request_trailers_dispatch) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string body;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(
                ruvia::HttpKnownMethod::kPost,
                std::pmr::string("/echo", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &echoHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4, 0, 0, {}))) co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "POST");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/echo");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(
                    0x1, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }
            // Body, then a trailing HEADERS block carrying END_STREAM.
            if (!co_await writeAll(frame(0x0, 0, 1, "hi"))) co_return;
            std::pmr::string trailerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(trailerBlock, "x-checksum", "abc");
            if (!co_await writeAll(frame(
                    0x1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
                    1, std::string_view(trailerBlock.data(), trailerBlock.size())))) {
                co_return;
            }

            for (;;) {
                char hb[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(hb, sizeof(hb))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(hb, sizeof(hb)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    header.streamId == 1 && !payload.empty()) {
                    body = payload;
                    break;
                }
            }
            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(body == "handler-ran");  // trailers ended the request; handler dispatched
}

// #14 regression: a large BUFFERED response paced over a small send window must
// deliver every byte + END_STREAM (and the core never buffers more than one slice).
RUVIA_TEST(sansio_driver_h2_large_buffered_body_paces_and_completes) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::uint64_t received = 0;
    bool sawEnd = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(
                ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/big", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &largeBufferedHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            const char settingsPayload[6] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x20};  // window 32
            if (!co_await writeAll(frame(0x4, 0, 0, std::string_view(settingsPayload, 6)))) co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/big");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(
                    0x1, ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders,
                    1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            for (;;) {
                char hb[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(hb, sizeof(hb))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(hb, sizeof(hb)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.streamId != 1 ||
                    header.type != static_cast<std::uint8_t>(Http2FrameType::kData)) {
                    continue;
                }
                received += payload.size();
                if ((header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                    sawEnd = true;
                    break;
                }
                if (!payload.empty()) {
                    char updates[2 * (ruvia::detail::kHttp2FrameHeaderBytes + 4)];
                    char* out = ruvia::detail::http2WriteWindowUpdate(
                        updates, 0, static_cast<std::uint32_t>(payload.size()));
                    out = ruvia::detail::http2WriteWindowUpdate(
                        out, 1, static_cast<std::uint32_t>(payload.size()));
                    if (!co_await writeAll(std::string_view(
                            updates, static_cast<std::size_t>(out - updates)))) {
                        break;
                    }
                }
            }
            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(received, static_cast<std::uint64_t>(kLargeBufferedBytes));
    RUVIA_CHECK(sawEnd);
}

// #1 regression: TWO concurrent WebSocket tunnels (two Extended-CONNECT streams) on
// ONE h2 connection must both work -- each registers its own heartbeat slot on the
// shared scanner entry, and both echo. Before the per-tunnel heartbeat-slot fix they
// clobbered each other's registration; this proves multiplexed tunnels coexist.
RUVIA_TEST(sansio_driver_h2_two_concurrent_ws_tunnels) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string echo1;
    std::string echo3;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerStreamRoute(
                ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsEchoHandler),
                ruvia::detail::ResponseBodyMode::kWebSocket,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto openTunnel = [](std::uint32_t streamId) {
                std::pmr::string block(std::pmr::get_default_resource());
                HpackEncoder::encodeHeader(block, ":method", "CONNECT");
                HpackEncoder::encodeHeader(block, ":protocol", "websocket");
                HpackEncoder::encodeHeader(block, ":scheme", "http");
                HpackEncoder::encodeHeader(block, ":path", "/ws");
                HpackEncoder::encodeHeader(block, ":authority", "localhost");
                HpackEncoder::encodeHeader(block, "sec-websocket-version", "13");
                return frame(0x1, ruvia::detail::kHttp2FlagEndHeaders, streamId,
                             std::string_view(block.data(), block.size()));
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4, 0, 0, {}))) co_return;
            // Open BOTH tunnels (streams 1 and 3) before sending any frames.
            if (!co_await writeAll(openTunnel(1))) co_return;
            if (!co_await writeAll(openTunnel(3))) co_return;
            // A masked text frame down each tunnel.
            if (!co_await writeAll(frame(0x0, 0, 1, maskedWsFrame(0x1, "one")))) co_return;
            if (!co_await writeAll(frame(0x0, 0, 3, maskedWsFrame(0x1, "three")))) co_return;

            std::string tunnel1;
            std::string tunnel3;
            auto extractFrame = [](std::string& acc) -> std::string {
                if (acc.size() < 2) return {};
                const auto len = static_cast<std::size_t>(
                    static_cast<unsigned char>(acc[1]) & 0x7FU);
                if (acc.size() < 2 + len) return {};
                return acc.substr(0, 2 + len);
            };
            while (echo1.empty() || echo3.empty()) {
                char hb[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(hb, sizeof(hb))) co_return;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(hb, sizeof(hb)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) co_return;
                if (header.type != static_cast<std::uint8_t>(Http2FrameType::kData)) continue;
                if (header.streamId == 1) {
                    tunnel1 += payload;
                    if (echo1.empty()) echo1 = extractFrame(tunnel1);
                } else if (header.streamId == 3) {
                    tunnel3 += payload;
                    if (echo3.empty()) echo3 = extractFrame(tunnel3);
                }
            }
            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(echo1.size() == 5 && echo1.substr(2) == "one");     // FIN|text len3 "one"
    RUVIA_CHECK(echo3.size() == 7 && echo3.substr(2) == "three");   // FIN|text len5 "three"
}
