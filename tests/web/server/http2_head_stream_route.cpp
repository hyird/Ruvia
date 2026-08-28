// An explicit HEAD streaming route over HTTP/2 must answer the streaming head
// with END_STREAM on the HEADERS frame and zero DATA frames, while GET on the
// same route still streams its body. Streaming GET routes do not receive
// implicit HEAD shadows; this drives the real sans-I/O h2 server session over a
// socket pair.

#include "http2_sansio_session_fixture.h"

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
#include <cstdio>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>

#include <zlib.h>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"
#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/util/CallableRef.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"

namespace {

using asio::ip::tcp;
using namespace ruvia::detail;

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

ruvia::Task<void> tickStreamHandler(void*, ruvia::Context& context) {
    co_await context.stream().write("tick-1");
    co_await context.stream().write("tick-2");
    co_await context.stream().end();
}

std::string frame(std::uint8_t type, std::uint8_t flags, std::uint32_t streamId, std::string_view payload) {
    std::string bytes(kHttp2FrameHeaderBytes, '\0');
    http2WriteFrameHeader(bytes.data(), static_cast<std::uint32_t>(payload.size()), static_cast<Http2FrameType>(type), flags, streamId);
    bytes.append(payload);
    return bytes;
}

struct StreamResult {
    std::string status;
    std::string contentEncoding;
    std::string body;
    bool sawData{false};
    bool ended{false};
};

[[nodiscard]] std::string gzipDecode(std::string_view encoded) {
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 32) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(encoded.data()));
    stream.avail_in = static_cast<uInt>(encoded.size());
    std::string decoded;
    char buffer[1024];
    for (;;) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        const auto status = inflate(&stream, Z_NO_FLUSH);
        decoded.append(buffer, sizeof(buffer) - stream.avail_out);
        if (status == Z_STREAM_END) {
            (void)inflateEnd(&stream);
            return decoded;
        }
        if (status != Z_OK || (stream.avail_in == 0 && stream.avail_out != 0)) {
            (void)inflateEnd(&stream);
            return {};
        }
    }
}

}  // namespace

int main() {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    std::pmr::string eventsPath("/events", std::pmr::get_default_resource());
    impl.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string(eventsPath, std::pmr::get_default_resource()), ruvia::detail::RouteStreamHandler(nullptr, &tickStreamHandler), {}, {});
    impl.registerResponseStreamRoute(ruvia::HttpKnownMethod::kHead, std::move(eventsPath), ruvia::detail::RouteStreamHandler(nullptr, &tickStreamHandler), {}, {});
    auto emptyHandler = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        context.status(ruvia::http_status::kNoContent);
        co_return context.body(nullptr);
    };
    impl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/empty", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(emptyHandler), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    impl.finalize();
    const auto& routes = impl.routeTable();

    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::test::Http2SansIoSessionFixture fixture;
            fixture.options.compression.emplace();
            auto dispatcher = std::make_shared<WorkerDispatcher>(io, 64);
            const auto workerHandle = WorkerHandleAccess::make(dispatcher);
            co_await taskAsAwaitable(runHttp2SansIoSession(sock, routes, worker, fixture.context(ContextServices(workerHandle).withPlainTransport("127.0.0.1"))));
        },
        asio::detached);

    StreamResult getStream;
    StreamResult headStream;
    StreamResult rejectedStream;
    StreamResult emptyStream;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto requestHeaders = [&writeAll](std::string_view method, std::string_view path, std::uint32_t streamId, bool rejectAllCodings) -> asio::awaitable<bool> {
                std::pmr::string headerBlock(std::pmr::get_default_resource());
                HpackEncoder::encodeHeader(headerBlock, ":method", method);
                HpackEncoder::encodeHeader(headerBlock, ":path", path);
                HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
                HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
                HpackEncoder::encodeHeader(headerBlock, "accept-encoding", rejectAllCodings ? "identity;q=0, gzip;q=0, br;q=0, zstd;q=0" : "gzip");
                co_return co_await writeAll(frame(0x1 /*HEADERS*/, kHttp2FlagEndStream | kHttp2FlagEndHeaders, streamId, std::string_view(headerBlock.data(), headerBlock.size())));
            };

            if (!co_await writeAll(kClientPreface)) {
                co_return;
            }
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }
            if (!co_await requestHeaders("GET", "/events", 1, false)) {
                co_return;
            }
            if (!co_await requestHeaders("HEAD", "/events", 3, false)) {
                co_return;
            }
            if (!co_await requestHeaders("GET", "/events", 5, true)) {
                co_return;
            }
            if (!co_await requestHeaders("GET", "/empty", 7, true)) {
                co_return;
            }

            HpackDecoder decoder({.resource = std::pmr::get_default_resource()});
            while (!getStream.ended || !headStream.ended || !rejectedStream.ended || !emptyStream.ended) {
                char headerBytes[kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                    break;
                }
                const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    break;
                }
                StreamResult* stream = header.streamId == 1 ? &getStream : header.streamId == 3 ? &headStream : header.streamId == 5 ? &rejectedStream : header.streamId == 7 ? &emptyStream : nullptr;
                if (stream == nullptr) {
                    continue;
                }
                if (header.type == 0x1 /*HEADERS*/) {
                    (void)decoder.decode(std::string_view(payload.data(), payload.size()), stream, [](void* target, std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            static_cast<StreamResult*>(target)->status = std::string(value);
                        } else if (name == "content-encoding") {
                            static_cast<StreamResult*>(target)->contentEncoding = std::string(value);
                        }
                        return true;
                    });
                } else if (header.type == 0x0 /*DATA*/) {
                    stream->sawData = true;
                    stream->body.append(payload);
                }
                if ((header.flags & kHttp2FlagEndStream) != 0 && (header.type == 0x0 || header.type == 0x1)) {
                    stream->ended = true;
                }
            }
            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();

    if (getStream.status != "200") {
        std::fprintf(stderr, "streaming GET over HTTP/2 was not 200: status='%s'\n", getStream.status.c_str());
        return 1;
    }
    if (getStream.contentEncoding != "gzip") {
        std::fprintf(stderr, "streaming GET over HTTP/2 did not negotiate gzip: '%s'\n", getStream.contentEncoding.c_str());
        return 2;
    }
    if (gzipDecode(getStream.body) != "tick-1tick-2") {
        std::fprintf(stderr, "streaming GET body was not a valid gzip stream over HTTP/2\n");
        return 3;
    }
    if (headStream.status != "200") {
        std::fprintf(stderr, "explicit HEAD of a streaming route over HTTP/2 was not 200: status='%s'\n", headStream.status.c_str());
        return 4;
    }
    if (headStream.contentEncoding != "gzip") {
        std::fprintf(stderr, "explicit HEAD of a streaming route over HTTP/2 lost gzip: '%s'\n", headStream.contentEncoding.c_str());
        return 5;
    }
    if (headStream.sawData || !headStream.body.empty()) {
        std::fprintf(stderr, "explicit HEAD of a streaming route over HTTP/2 must send no DATA, got '%s'\n", headStream.body.c_str());
        return 6;
    }
    if (rejectedStream.status != "406") {
        std::fprintf(stderr, "an empty response coding set over HTTP/2 was not rejected with 406: status='%s'\n", rejectedStream.status.c_str());
        return 7;
    }
    if (emptyStream.status != "204" || emptyStream.sawData || !emptyStream.body.empty()) {
        std::fprintf(stderr,
            "a bodyless buffered response over HTTP/2 was rejected by empty coding negotiation: "
            "status='%s' body='%s'\n",
            emptyStream.status.c_str(), emptyStream.body.c_str());
        return 8;
    }
    return 0;
}
