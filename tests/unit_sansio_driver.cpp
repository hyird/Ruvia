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

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "HttpRequestInternal.h"
#include "HttpResponseBodyAccess.h"
#include "http/ContextServices.h"
#include "net/http2/Http2Connection.h"
#include "net/http2/Http2FrameCodec.h"
#include "net/http2/Http2Hpack.h"
#include "net/http2/Http2RequestBuilder.h"
#include "net/server/Http2SansIoSession.h"
#include "router/RouteResolution.h"
#include "router/RouterInternal.h"
#include "router/RouteTable.h"
#include "runtime/AsioAwait.h"
#include "runtime/SansIoDriver.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/memory/MemoryPool.h"
#include "ruvia/router/Router.h"

namespace {

using asio::ip::tcp;
using ruvia::detail::Http2Connection;
using ruvia::detail::Http2Event;
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
    co_await ruvia::detail::asyncError([&timer](auto handler) mutable {
        timer.async_wait(std::move(handler));
    });
    co_return ctx.text("slow");
}

ruvia::Task<ruvia::HttpResponse> fastHandler(void*, ruvia::Context& ctx) {
    co_return ctx.text("fast");
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

// Build a masked client->server WebSocket frame (RFC 6455 §5.1, short lengths only).
std::string maskedWsFrame(std::uint8_t opcode, std::string_view payload) {
    std::string f;
    f.push_back(static_cast<char>(0x80U | opcode));  // FIN | opcode
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
            conn.expectClientPreface();
            conn.queueLocalSettings();

            auto onReadable = [&resource](Http2Connection& c) -> ruvia::Task<void> {
                for (;;) {
                    const auto event = c.nextEvent();
                    if (event.kind == Http2Event::Kind::kNone) {
                        break;
                    }
                    if (event.kind == Http2Event::Kind::kRequestEnd) {
                        ruvia::HttpResponse response(&resource);
                        response.status(200);
                        response.setBodyCopy("pong");
                        c.submitResponseHead(event.streamId, response, /*bodyForbidden=*/false);
                        c.submitData(event.streamId, "pong", /*endStream=*/true);
                    }
                }
                co_return;
            };
            co_await ruvia::detail::taskAsAwaitable(
                ruvia::detail::pumpSansIoConnection(conn, sock, onReadable));
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
                ruvia::detail::runHttp2SansIoBufferedSession(sock, routes, worker, "127.0.0.1"));
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
                ruvia::HttpMethod::kPost,
                std::pmr::string("/echo", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &echoHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoBufferedSession(
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
                ruvia::HttpMethod::kGet,
                std::pmr::string("/slow", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(&io, &slowHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.registerRoute(
                ruvia::HttpMethod::kGet,
                std::pmr::string("/fast", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &fastHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoBufferedSession(
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
                ruvia::HttpMethod::kGet,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsEchoHandler),
                ruvia::detail::ResponseBodyMode::kWebSocket,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoBufferedSession(
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

            // Close the tunnel: masked Close (1000) -> the server echoes a Close and
            // half-closes the stream (END_STREAM).
            if (!co_await writeAll(frame(
                    0x0 /*DATA*/, 0, 1, maskedWsFrame(0x8, std::string_view("\x03\xE8", 2))))) {
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
                ruvia::HttpMethod::kGet,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsEchoHandler),
                ruvia::detail::ResponseBodyMode::kWebSocket,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoBufferedSession(
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
