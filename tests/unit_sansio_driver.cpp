#include "test_harness.h"

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "net/http2/Http2Connection.h"
#include "net/http2/Http2FrameCodec.h"
#include "net/http2/Http2Hpack.h"
#include "runtime/AsioAwait.h"
#include "runtime/SansIoDriver.h"
#include "ruvia/http/HttpResponse.h"

namespace {

using asio::ip::tcp;
using ruvia::detail::Http2Connection;
using ruvia::detail::Http2Event;
using ruvia::detail::Http2FrameType;
using ruvia::detail::HpackEncoder;

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

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
