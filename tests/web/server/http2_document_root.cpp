// A static file from the configured document root must be served over HTTP/2,
// not only HTTP/1: the sans-I/O h2 session used to skip the document-root
// fallback that the HTTP/1 session applies, so files reachable over HTTP/1
// returned 404 over HTTP/2. Drives the real h2 server session over a socket
// pair and asserts a GET for an unrouted path yields the file's bytes, and that
// a HEAD for the same path yields 200 with the metadata but no DATA body.

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
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"
#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"

namespace {

using asio::ip::tcp;
using namespace ruvia::detail;

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::string_view kFileBody = "hello-static-file-over-h2";

std::string frame(std::uint8_t type, std::uint8_t flags, std::uint32_t streamId, std::string_view payload) {
    std::string bytes(kHttp2FrameHeaderBytes, '\0');
    http2WriteFrameHeader(bytes.data(), static_cast<std::uint32_t>(payload.size()), static_cast<Http2FrameType>(type), flags, streamId);
    bytes.append(payload);
    return bytes;
}

struct StreamResult {
    std::string status;
    std::string body;
    bool sawData{false};
    bool ended{false};
};

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_h2_docroot_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "asset.txt");
        f << kFileBody;
    }
    ruvia::StaticRoot root(dir, {});

    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    // Server: the real h2 session with an empty route table but a document root,
    // so an unrouted path falls back to the static file.
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::RouteTable routes(worker.resource());
            ruvia::test::Http2SansIoSessionFixture fixture;
            fixture.options.documentRoot.root = &root;
            auto dispatcher = std::make_shared<WorkerDispatcher>(io, 64);
            const auto workerHandle = WorkerHandleAccess::make(dispatcher);
            co_await taskAsAwaitable(runHttp2SansIoSession(sock, routes, worker, fixture.context(ContextServices{}.withPlainTransport("127.0.0.1").withWorker(workerHandle))));
        },
        asio::detached);

    StreamResult getStream;
    StreamResult headStream;

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
            auto requestHeaders = [&writeAll](std::string_view method, std::uint32_t streamId) -> asio::awaitable<bool> {
                std::pmr::string headerBlock(std::pmr::get_default_resource());
                HpackEncoder::encodeHeader(headerBlock, ":method", method);
                HpackEncoder::encodeHeader(headerBlock, ":path", "/asset.txt");
                HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
                HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
                co_return co_await writeAll(frame(0x1 /*HEADERS*/, kHttp2FlagEndStream | kHttp2FlagEndHeaders, streamId, std::string_view(headerBlock.data(), headerBlock.size())));
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;
            if (!co_await requestHeaders("GET", 1)) co_return;
            if (!co_await requestHeaders("HEAD", 3)) co_return;

            HpackDecoder decoder(std::pmr::get_default_resource());
            while (!getStream.ended || !headStream.ended) {
                char headerBytes[kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    break;
                }
                StreamResult* stream = header.streamId == 1 ? &getStream : header.streamId == 3 ? &headStream : nullptr;
                if (stream == nullptr) {
                    continue;
                }
                if (header.type == 0x1 /*HEADERS*/) {
                    (void)decoder.decode(std::string_view(payload.data(), payload.size()), stream, [](void* target, std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            static_cast<StreamResult*>(target)->status = std::string(value);
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
    fs::remove_all(dir);

    if (getStream.status != "200") {
        std::fprintf(stderr, "document root not served over HTTP/2: GET status='%s'\n", getStream.status.c_str());
        return 1;
    }
    if (getStream.body != kFileBody) {
        std::fprintf(stderr, "HTTP/2 document-root body mismatch: '%s'\n", getStream.body.c_str());
        return 2;
    }
    // HEAD must answer 200 for the same file GET serves, but carry no DATA body.
    if (headStream.status != "200") {
        std::fprintf(stderr, "HEAD of a document-root file over HTTP/2 was not 200: status='%s'\n", headStream.status.c_str());
        return 3;
    }
    if (headStream.sawData || !headStream.body.empty()) {
        std::fprintf(stderr, "HEAD over HTTP/2 must send no body, got '%s'\n", headStream.body.c_str());
        return 4;
    }
    return 0;
}
