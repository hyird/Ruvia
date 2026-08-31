// The HTTP/2 twin of web/server/connection_failure.cpp. A stream handler that
// throws after committing its head can only be answered with RST_STREAM, which
// tells the peer that the stream died but not why -- and nothing downstream
// still holds the reason. This asserts the exception reaches the connection-
// failure listener, and that the peer really is reset.
//
// Drives the real sans-I/O h2 server session over a loopback socket.

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
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"
#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/ServerConfig.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"

namespace {

using asio::ip::tcp;
using namespace ruvia::detail;

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// Commits the head and one DATA frame, then fails: everything before the throw
// is already on the wire.
ruvia::Task<void> failingStreamHandler(void*, ruvia::Context& context) {
    co_await context.stream().write("partial");
    throw std::runtime_error("h2 handler failed mid-stream");
}

struct FailureObservation final {
    std::size_t calls{0};
    std::string message;
    std::string remoteAddress;

    void operator()(const ruvia::ConnectionFailureRecord& record) noexcept {
        ++calls;
        try {
            remoteAddress.assign(record.remoteAddress());
            std::rethrow_exception(record.exception());
        } catch (const std::exception& error) {
            message.assign(error.what());
        } catch (...) {
            message.assign("<unknown>");
        }
    }
};

std::string frame(
    std::uint8_t type, std::uint8_t flags, std::uint32_t streamId, std::string_view payload) {
    std::string bytes(kHttp2FrameHeaderBytes, '\0');
    http2WriteFrameHeader(bytes.data(), static_cast<std::uint32_t>(payload.size()),
        static_cast<Http2FrameType>(type), flags, streamId);
    bytes.append(payload);
    return bytes;
}

}  // namespace

int main() {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    std::pmr::string boomPath("/boom", std::pmr::get_default_resource());
    impl.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet, std::move(boomPath),
        ruvia::detail::RouteStreamHandler(nullptr, &failingStreamHandler), {}, {});
    impl.finalize();
    const auto& routes = impl.routeTable();

    FailureObservation observation;

    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::test::Http2SansIoSessionFixture fixture;
            fixture.options.connectionFailure.callback = ruvia::detail::CallbackAccess::bind<void(
                const ruvia::ConnectionFailureRecord&) noexcept>(observation);
            auto dispatcher = std::make_shared<WorkerDispatcher>(io, 64);
            const auto workerHandle = WorkerHandleAccess::make(dispatcher);
            co_await taskAsAwaitable(runHttp2SansIoSession(sock, routes, worker,
                fixture.context(fixture.services(workerHandle).withPlainTransport("127.0.0.1"))));
        },
        asio::detached);

    bool sawReset = false;
    bool sawHead = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) {
                co_return;
            }
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/boom");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(
                    frame(0x1 /*HEADERS*/, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1,
                        std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            // Read until the stream is reset or the peer goes away.
            for (;;) {
                char headerBytes[kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                    break;
                }
                const auto header =
                    http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    break;
                }
                if (header.streamId != 1) {
                    continue;
                }
                if (header.type == 0x1 /*HEADERS*/) {
                    sawHead = true;
                } else if (header.type == 0x3 /*RST_STREAM*/) {
                    sawReset = true;
                    break;
                }
            }
            std::error_code ignore;
            sock.close(ignore);
        },
        asio::detached);

    io.run();

    int rc = 0;
    auto fail = [&](int code, const char* message) {
        std::fputs(message, stderr);
        std::fputc('\n', stderr);
        rc = code;
    };

    if (!sawHead) {
        fail(1, "the failing h2 stream route never committed its head");
    } else if (!sawReset) {
        fail(2, "a committed h2 stream failure did not reset the stream");
    } else if (observation.calls != 1) {
        fail(3, "the h2 connection failure did not reach the listener exactly once");
    } else if (observation.message != "h2 handler failed mid-stream") {
        fail(4, "the listener did not receive the original h2 exception");
    } else if (observation.remoteAddress != "127.0.0.1") {
        fail(5, "the h2 failure did not carry the peer address");
    }
    return rc;
}

