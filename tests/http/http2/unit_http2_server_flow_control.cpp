#include "test_harness.h"
#include "http2_sansio_session_fixture.h"

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
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2FrameTypes.h"
#include "ruvia/http/detail/http2/Http2Hpack.h"
#include "ruvia/http/detail/http2/Http2LocalSettings.h"
#include "ruvia/http/detail/http2/Http2ReceiveWindowCredit.h"
#include "ruvia/http/detail/HttpResponseFileAccess.h"
// The production session header owns every declaration needed by its templates;
// this test intentionally must not rely on a server-wide include-order umbrella.
#include "ruvia/web/detail/server/Http2SansIoSession.h"
#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/memory/MemoryPool.h"

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

// Drives the real sans-I/O h2 server session as the peer: completes the handshake, sends a
// body-less request on stream 1, then sends DATA on that now-ended stream. Those
// frames are dropped by the server (RFC 9113 6.9.1: they still count against the
// connection flow-control window). Returns every connection-level (stream 0)
// WINDOW_UPDATE increment emitted by the real session.
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
            co_await ruvia::detail::taskAsAwaitable(
                ruvia::test::runBarePlainHttp2SansIoSession(
                    sock, routes, worker, "127.0.0.1"));
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

            // DATA on the now-ended stream: every frame must be dropped yet still
            // join the connection credit batch. Keep each payload within the local
            // SETTINGS_MAX_FRAME_SIZE advertised by the server.
            std::string data(Http2LocalSettings::kMaxFrameSize, 'x');
            auto remaining = dataBytes;
            while (remaining != 0) {
                const auto chunkBytes = static_cast<std::size_t>(
                    remaining < data.size() ? remaining : data.size());
                if (!co_await writeAll(frame(
                        0x0 /*DATA*/,
                        0,
                        1,
                        std::string_view(data.data(), chunkBytes)))) {
                    co_return;
                }
                remaining -= static_cast<std::uint32_t>(chunkBytes);
            }

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
            co_await ruvia::detail::taskAsAwaitable(
                ruvia::test::runBarePlainHttp2SansIoSession(
                    sock, routes, worker, "127.0.0.1"));
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

// A not-found handler whose response carries a header value large enough that the
// HPACK-encoded response header block exceeds the default 16384-byte max frame size,
// forcing the server onto its HEADERS + CONTINUATION path.
ruvia::Task<ruvia::HttpResponse> largeHeaderNotFoundHandler(ruvia::Context& context) {
    (void)context;
    ruvia::HttpResponse response(std::pmr::get_default_resource());
    response.status(ruvia::http_status::kNotFound);
    static const std::string bigValue(40000, 'a');
    response.header("x-large", bigValue);
    co_return response;
}

struct EmittedFrame {
    std::uint8_t type;
    std::uint32_t streamId;
    std::uint8_t flags;
};

// Two requests (streams 1 and 3) arrive together; both 404 into the large-header
// handler, so both produce a multi-frame (HEADERS + CONTINUATION) response header
// block and their writes contend for the single write turn. Returns every frame the
// server emits, so the test can assert RFC 9113 6.10: a stream's HEADERS + CONTINUATION
// frames must be contiguous on the wire, never interleaved with another stream's frame.
std::vector<EmittedFrame> framesForConcurrentLargeHeaderResponses() {
    asio::io_context io;
    std::vector<EmittedFrame> frames;

    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::RouteTable routes(worker.resource());
            routes.setNotFoundHandler(&largeHeaderNotFoundHandler);
            co_await ruvia::detail::taskAsAwaitable(
                ruvia::test::runBarePlainHttp2SansIoSession(
                    sock, routes, worker, "127.0.0.1"));
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
            HpackEncoder::encodeHeader(headerBlock, ":path", "/");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            const auto reqView = std::string_view(headerBlock.data(), headerBlock.size());

            // Both requests in a single write, so the server dispatches them together.
            std::string both;
            both += frame(0x1 /*HEADERS*/, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1, reqView);
            both += frame(0x1 /*HEADERS*/, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 3, reqView);
            if (!co_await writeAll(both)) co_return;

            asio::error_code shutdownError;
            sock.shutdown(tcp::socket::shutdown_send, shutdownError);

            std::size_t completedResponses = 0;
            for (;;) {
                char headerBytes[kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                frames.push_back(EmittedFrame{
                    static_cast<std::uint8_t>(header.type), header.streamId, header.flags});
                if ((header.type == 0x1 /*HEADERS*/ ||
                     header.type == 0x9 /*CONTINUATION*/) &&
                    (header.flags & kHttp2FlagEndHeaders) != 0 &&
                    (header.streamId == 1 || header.streamId == 3) &&
                    ++completedResponses == 2) {
                    break;
                }
            }
            asio::error_code ignore;
            sock.close(ignore);
            io.stop();
        },
        asio::detached);

    io.run();
    return frames;
}

// The on-disk path of the short file the truncated-file-body handler serves. Set by
// rstErrorForTruncatedFileBody() before the server runs; the single-threaded io_context
// makes this static safe.
std::string& truncatedFileBodyPath() {
    static std::string path;
    return path;
}

// A handler whose file-body response advertises a length far larger than the bytes
// actually on disk. writeFileBody streams the real (short) file, then hits EOF while
// it still owes body bytes -- the file-truncated/removed-mid-serve case -- so it can
// no longer honour the content-length it already sent.
ruvia::Task<ruvia::HttpResponse> truncatedFileBodyHandler(ruvia::Context& context) {
    (void)context;
    ruvia::HttpResponse response(std::pmr::get_default_resource());
    response.status(ruvia::http_status::kOk);
    constexpr std::uint64_t declaredLength = 40000;
    ruvia::detail::setResponseFileBody(
        response, std::filesystem::path(truncatedFileBodyPath()), declaredLength, 0, declaredLength);
    co_return response;
}

// The on-disk path (guaranteed absent) the missing-file-body handler points at.
std::string& missingFileBodyPath() {
    static std::string path;
    return path;
}

// A handler whose file-body response references a file that does not exist on disk,
// so openResponseFileInput fails after the response headers (with content-length) have
// already been sent -- the file-removed-before-serve case.
ruvia::Task<ruvia::HttpResponse> missingFileBodyHandler(ruvia::Context& context) {
    (void)context;
    ruvia::HttpResponse response(std::pmr::get_default_resource());
    response.status(ruvia::http_status::kOk);
    constexpr std::uint64_t declaredLength = 40000;
    ruvia::detail::setResponseFileBody(
        response, std::filesystem::path(missingFileBodyPath()), declaredLength, 0, declaredLength);
    co_return response;
}

// Drives the real sans-I/O h2 session over loopback whose not-found handler returns a
// file-body response that cannot be delivered (truncated on disk, or the file is
// missing). Returns the RST_STREAM error code the server sends for stream 1, or
// std::nullopt if it emitted no RST. The connection is deliberately left open (no
// shutdown_send) so the server must abort the stream while the connection is still
// live; a watchdog closes the socket if the RST never arrives, so the neutered-fix
// (mutation) case fails fast instead of blocking on the read forever.
std::optional<std::uint32_t> rstErrorForFileBodyHandler(
    ruvia::Task<ruvia::HttpResponse> (*handler)(ruvia::Context&)) {
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
            routes.setNotFoundHandler(handler);
            co_await ruvia::detail::taskAsAwaitable(
                ruvia::test::runBarePlainHttp2SansIoSession(
                    sock, routes, worker, "127.0.0.1"));
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

            asio::steady_timer watchdog(io);
            watchdog.expires_after(std::chrono::seconds(5));
            watchdog.async_wait([&sock](const asio::error_code& ec) {
                if (!ec) {
                    asio::error_code ignore;
                    sock.close(ignore);
                }
            });

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
                    break;
                }
            }
            watchdog.cancel();
            asio::error_code ignore;
            sock.close(ignore);
        },
        asio::detached);

    io.run();
    return rstError;
}

// Mid-body truncation: the served file is much shorter than the advertised length.
std::optional<std::uint32_t> rstErrorForTruncatedFileBody() {
    const auto filePath =
        std::filesystem::temp_directory_path() / "ruvia_h2_truncated_body_test.bin";
    {
        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        out << "short";  // 5 bytes on disk vs 40000 advertised
    }
    truncatedFileBodyPath() = filePath.string();
    const auto rstError = rstErrorForFileBodyHandler(&truncatedFileBodyHandler);
    std::error_code removeError;
    std::filesystem::remove(filePath, removeError);
    return rstError;
}

// File-open failure: the referenced file does not exist when the body is served.
std::optional<std::uint32_t> rstErrorForMissingFileBody() {
    const auto filePath =
        std::filesystem::temp_directory_path() / "ruvia_h2_missing_body_test.bin";
    std::error_code removeError;
    std::filesystem::remove(filePath, removeError);  // ensure absent
    missingFileBodyPath() = filePath.string();
    return rstErrorForFileBodyHandler(&missingFileBodyHandler);
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
    // DATA sent on an ended stream is dropped, but RFC 9113 6.9.1 still requires
    // its bytes to be returned at connection scope. The real session batches that
    // credit to avoid per-frame output amplification, then restores one exact
    // threshold when reached.
    constexpr auto threshold = kHttp2ReceiveWindowUpdateThreshold;
    const auto increments =
        collectConnectionWindowUpdatesForDroppedData(threshold);
    bool credited = false;
    for (const auto increment : increments) {
        if (increment == threshold) {
            credited = true;
        }
    }
    RUVIA_CHECK(credited);
}

RUVIA_TEST(http2_headers_and_continuation_not_interleaved_across_streams) {
    const auto frames = framesForConcurrentLargeHeaderResponses();

    // The oversized response headers must have exercised the multi-frame path.
    bool sawContinuation = false;
    for (const auto& f : frames) {
        if (f.type == 0x9 /*CONTINUATION*/) {
            sawContinuation = true;
        }
    }
    RUVIA_CHECK(sawContinuation);

    // RFC 9113 6.10: once a HEADERS frame without END_HEADERS opens a header block,
    // every frame until END_HEADERS must be a CONTINUATION on the SAME stream -- no
    // other stream's frame may interleave.
    std::uint32_t openStream = 0;
    bool interleaved = false;
    for (const auto& f : frames) {
        if (openStream != 0) {
            if (f.type != 0x9 /*CONTINUATION*/ || f.streamId != openStream) {
                interleaved = true;
                break;
            }
            if ((f.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0) {
                openStream = 0;
            }
        } else if (f.type == 0x1 /*HEADERS*/ &&
                   (f.flags & ruvia::detail::kHttp2FlagEndHeaders) == 0) {
            openStream = f.streamId;
        }
    }
    RUVIA_CHECK(!interleaved);
    RUVIA_CHECK_EQ(openStream, std::uint32_t{0});
}

RUVIA_TEST(http2_truncated_file_body_aborts_stream_with_rst) {
    // The response advertises content-length 40000 but the file on disk is 5 bytes, so
    // writeFileBody hits a short read (read<=0) with body bytes still outstanding and
    // can no longer honour the advertised length. RFC 9113 §8.1: a committed response
    // that cannot finish must terminate the stream. Before the fix writeFileBody just
    // returned, leaving the stream open with no END_STREAM and no RST_STREAM, so the
    // peer hung until its own timeout. Expect RST_STREAM(INTERNAL_ERROR = 0x2).
    const auto rstError = rstErrorForTruncatedFileBody();
    RUVIA_CHECK(rstError.has_value());
    RUVIA_CHECK_EQ(rstError.value_or(0), std::uint32_t{0x2});
}

RUVIA_TEST(http2_missing_file_body_aborts_stream_with_rst) {
    // The response advertises content-length 40000 but the file is gone by the time the
    // body is served, so openResponseFileInput fails after the headers are already on
    // the wire. Sending DATA(0, END_STREAM) would be a content-length mismatch (RFC 9113
    // §8.1.1) that a lenient peer accepts as a valid empty body -- silently turning a
    // failed serve into a 200 with no content. Before the fix writeFileBody did exactly
    // that; now it aborts the stream so the peer learns the response failed. Expect
    // RST_STREAM(INTERNAL_ERROR = 0x2), matching the mid-body truncation path.
    const auto rstError = rstErrorForMissingFileBody();
    RUVIA_CHECK(rstError.has_value());
    RUVIA_CHECK_EQ(rstError.value_or(0), std::uint32_t{0x2});
}
