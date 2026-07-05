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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/http2/Http2FrameCodec.h"
#include "net/http2/Http2FrameTypes.h"
#include "net/http2/Http2Hpack.h"
// Instantiating Http2ServerSession pulls in its response-write .inl templates, which
// reference free-function helpers (ensureFileChunkBuffer, setRetryAfterSeconds, ...)
// by non-dependent name — so those must be visible first. Mirror the exact include
// order of the production instantiation TU (HttpServerAccept.cpp): HttpResponseWriter.h
// then the HttpServerSessionUtils.h umbrella (which pulls in the session header).
#include "net/server/HttpResponseWriter.h"
#include "net/server/HttpServerSessionUtils.h"
#include "net/server/ConnectionScanner.h"
#include "router/RouteTable.h"
#include "runtime/AsioAwait.h"
#include "ruvia/app/App.h"
#include "ruvia/memory/MemoryPool.h"

namespace {

using asio::ip::tcp;
using namespace ruvia::detail;

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

std::string frame(std::uint8_t type, std::uint8_t flags, std::uint32_t streamId, std::string_view payload) {
    std::string bytes(kHttp2FrameHeaderBytes, '\0');
    http2WriteFrameHeader(
        bytes.data(),
        static_cast<std::uint32_t>(payload.size()),
        static_cast<Http2FrameType>(type),
        flags,
        streamId);
    bytes.append(payload);
    return bytes;
}

// Drives a real Http2ServerSession as the peer: completes the handshake, sends a
// body-less request on stream 1, then sends a DATA frame on that now-ended stream.
// That DATA frame is dropped by the server (RFC 9113 6.9.1: it is counted against
// the connection flow-control window even though the stream is gone). Returns every
// connection-level (stream 0) WINDOW_UPDATE increment the server emits, so the test
// can assert the dropped frame's bytes were credited back to the peer.
std::vector<std::uint32_t> collectConnectionWindowUpdatesForDroppedData(std::uint32_t dataBytes) {
    asio::io_context io;
    std::vector<std::uint32_t> increments;

    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    // Server side: a real session with an empty route table (requests 404, which is
    // enough to open and finish the stream — the exact post-request stream state does
    // not matter, every drop branch must credit the connection window).
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::RouteTable routes(worker.resource());
            ruvia::HttpServerOptions options;
            ruvia::detail::ConnectionScanner::Entry scannerEntry;
            ruvia::detail::Http2ServerSession<tcp::socket> session(
                sock,
                sock,
                worker,
                routes,
                nullptr,
                nullptr,
                nullptr,
                options,
                scannerEntry,
                "127.0.0.1",
                nullptr);
            co_await ruvia::detail::taskAsAwaitable(session.run());
        },
        asio::detached);

    // Client side: our synthetic HTTP/2 peer.
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

            // Preface + empty client SETTINGS.
            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            // A complete, body-less request on stream 1 (END_STREAM + END_HEADERS).
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(
                    0x1 /*HEADERS*/,
                    kHttp2FlagEndStream | kHttp2FlagEndHeaders,
                    1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            // DATA on the now-ended stream: the frame the server must drop yet still
            // credit against the connection window.
            std::string data(dataBytes, 'x');
            if (!co_await writeAll(frame(0x0 /*DATA*/, 0, 1, data))) co_return;

            // Half-close so the server's read loop reaches EOF and tears down.
            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_send, ignore);

            // Drain every frame the server emits, recording stream-0 WINDOW_UPDATEs.
            for (;;) {
                char headerBytes[kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == 0x8 /*WINDOW_UPDATE*/ && header.streamId == 0) {
                    increments.push_back(http2Read31(
                        reinterpret_cast<const unsigned char*>(payload.data())));
                }
            }
        },
        asio::detached);

    io.run();
    return increments;
}

// Drives a real server with a request whose content-length is nonzero but whose
// HEADERS frame carries END_STREAM (so no DATA can follow). Returns the RST_STREAM
// error code the server sends for stream 1, or std::nullopt if the stream was
// accepted instead of rejected.
std::optional<std::uint32_t> rstErrorForBodylessContentLengthRequest() {
    asio::io_context io;
    std::optional<std::uint32_t> rstError;

    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::RouteTable routes(worker.resource());
            ruvia::HttpServerOptions options;
            ruvia::detail::ConnectionScanner::Entry scannerEntry;
            ruvia::detail::Http2ServerSession<tcp::socket> session(
                sock, sock, worker, routes, nullptr, nullptr, nullptr, options, scannerEntry,
                "127.0.0.1", nullptr);
            co_await ruvia::detail::taskAsAwaitable(session.run());
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

            // content-length: 5 with END_STREAM on HEADERS: no DATA can ever arrive,
            // so the declared length can never be satisfied -> malformed (RFC 9113
            // §8.1.1). The server must RST_STREAM(PROTOCOL_ERROR).
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "POST");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(headerBlock, "content-length", "5");
            if (!co_await writeAll(frame(
                    0x1 /*HEADERS*/,
                    kHttp2FlagEndStream | kHttp2FlagEndHeaders,
                    1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_send, ignore);

            for (;;) {
                char headerBytes[kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == 0x3 /*RST_STREAM*/ && header.streamId == 1 && payload.size() == 4) {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(payload.data());
                    rstError = (static_cast<std::uint32_t>(bytes[0]) << 24) |
                        (static_cast<std::uint32_t>(bytes[1]) << 16) |
                        (static_cast<std::uint32_t>(bytes[2]) << 8) |
                        static_cast<std::uint32_t>(bytes[3]);
                }
            }
        },
        asio::detached);

    io.run();
    return rstError;
}

}  // namespace

RUVIA_TEST(http2_bodyless_headers_with_content_length_is_rejected) {
    // RFC 9113 §8.1.1: content-length must equal the sum of DATA payload lengths.
    // END_STREAM on HEADERS means zero DATA, so a nonzero content-length is
    // unsatisfiable and the request is malformed. The DATA path enforced this at its
    // own END_STREAM, but a body-less HEADERS reached dispatch unchecked until the
    // fix. Expect RST_STREAM(PROTOCOL_ERROR = 0x1).
    const auto rstError = rstErrorForBodylessContentLengthRequest();
    RUVIA_CHECK(rstError.has_value());
    RUVIA_CHECK_EQ(rstError.value_or(0), std::uint32_t{0x1});
}

RUVIA_TEST(http2_dropped_data_credits_connection_flow_window) {
    // The distinctive 7-byte DATA frame is dropped (sent on an ended stream); RFC
    // 9113 6.9.1 still requires its bytes to be returned to the peer at the
    // connection level, or the peer's send window drains and stalls every stream.
    // Before the fix no stream-0 WINDOW_UPDATE carried this credit.
    const auto increments = collectConnectionWindowUpdatesForDroppedData(7);
    bool credited = false;
    for (const auto increment : increments) {
        if (increment == 7) {
            credited = true;
        }
    }
    RUVIA_CHECK(credited);
}
