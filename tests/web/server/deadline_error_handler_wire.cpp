// Deadline coverage for server-layer error handlers. These paths do not go
// through the ordinary handler body, so they specifically guard that the
// request-scoped ContextServices reaches onError after the server has already
// turned the request into a protocol/product rejection.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "http2_sansio_session_fixture.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"
#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Deadline.h"
#include "ruvia/web/RateLimitRule.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/detail/CallbackRef.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/HttpServer.h"
#include "ruvia/web/detail/util/CallableRef.h"

using namespace std::chrono_literals;

namespace {

using asio::ip::tcp;
using namespace ruvia::detail;

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr auto kDeadline = 100ms;
constexpr auto kMissedDeadlineWait = 3s;

struct H2Result final {
    std::string status;
    std::string body;
    bool ended{false};
    std::chrono::steady_clock::duration elapsed{};
};

std::string frame(std::uint8_t type, std::uint8_t flags, std::uint32_t streamId, std::string_view payload) {
    std::string bytes(kHttp2FrameHeaderBytes, '\0');
    http2WriteFrameHeader(bytes.data(), static_cast<std::uint32_t>(payload.size()), static_cast<Http2FrameType>(type), flags, streamId);
    bytes.append(payload);
    return bytes;
}

template <typename Handler>
void registerBufferedRoute(ruvia::detail::RouterImpl& router, std::string_view path, Handler& handler, std::span<const ruvia::detail::ControllerMiddlewareDescriptor> middlewares) {
    router.registerRoute(
        ruvia::HttpKnownMethod::kGet,
        std::pmr::string(path, std::pmr::get_default_resource()),
        ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(handler),
        ruvia::detail::RequestBodyMode::kBuffered,
        {},
        middlewares);
}

ruvia::Task<void> unusedStreamHandler(void*, ruvia::Context& context) {
    auto& stream = context.streamText();
    co_await stream.write("should-not-run");
    co_await stream.end();
}

struct DeadlineAwareErrorHandler final {
    ruvia::Task<ruvia::HttpResponse> operator()(ruvia::Context& context, ruvia::HttpErrorInfo) const {
        const auto result = co_await ruvia::sleepFor(context.worker(), kMissedDeadlineWait, context.stopToken());
        if (result == ruvia::TimerSleepResult::kStopRequested && context.deadlineExceeded()) {
            context.status(ruvia::http_status::kGatewayTimeout);
            co_return context.text("deadline");
        }
        context.status(ruvia::http_status::kOk);
        co_return context.text("slept");
    }
};

[[nodiscard]] std::string readStatusLine(asio::ip::tcp::socket& socket, std::error_code& ec) {
    asio::streambuf buffer;
    asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    const std::string head(asio::buffers_begin(buffer.data()), asio::buffers_end(buffer.data()));
    const auto lineEnd = head.find("\r\n");
    return head.substr(0, lineEnd == std::string::npos ? head.size() : lineEnd);
}

[[nodiscard]] std::string h1Exchange(const asio::ip::tcp::endpoint& endpoint, std::string_view request, std::chrono::steady_clock::duration& elapsed) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    std::error_code ec;
    socket.connect(endpoint, ec);
    if (ec) {
        return "connect failed";
    }
    const auto started = std::chrono::steady_clock::now();
    asio::write(socket, asio::buffer(request), ec);
    if (ec) {
        return "write failed";
    }
    auto status = readStatusLine(socket, ec);
    elapsed = std::chrono::steady_clock::now() - started;
    if (ec && status.empty()) {
        return "read failed";
    }
    return status;
}

[[nodiscard]] int checkDeadlineResponse(std::string_view label, std::string_view status, std::chrono::steady_clock::duration elapsed) {
    if (status.find("504") == std::string_view::npos) {
        std::fprintf(stderr, "%.*s error handler did not observe the request deadline: %.*s\n", static_cast<int>(label.size()), label.data(), static_cast<int>(status.size()), status.data());
        return 1;
    }
    if (elapsed > kMissedDeadlineWait / 2) {
        std::fprintf(stderr, "%.*s error handler answered only after its full wait\n", static_cast<int>(label.size()), label.data());
        return 2;
    }
    return 0;
}

[[nodiscard]] int runHttp1RateLimitErrorDeadline() {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);

    auto ok = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        co_return context.text("ok");
    };
    const auto deadline = ruvia::detail::makeMiddlewareDescriptor<ruvia::Deadline<static_cast<std::int64_t>(kDeadline.count())>>();
    registerBufferedRoute(impl, "/limited", ok, std::span(&deadline, std::size_t{1}));

    ruvia::HttpErrorHandler errorHandler(DeadlineAwareErrorHandler{});
    impl.setErrorHandler(ruvia::detail::CallbackAccess::ref(errorHandler));
    impl.finalize();

    ruvia::detail::HttpServerOptions options;
    options.defaultRateLimitPerWorker = ruvia::RateLimitRule::fixedWindow({
        .maxRequests = 1,
        .window = 60s,
    });

    ruvia::detail::HttpServer server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), impl.routeTable(), {}, options);
    server.start();
    const auto endpoint = server.localEndpoint();

    int rc = 0;
    std::chrono::steady_clock::duration elapsed{};
    const auto first = h1Exchange(endpoint, "GET /limited HTTP/1.1\r\nHost: x\r\n\r\n", elapsed);
    if (!first.starts_with("HTTP/1.1 200")) {
        std::fprintf(stderr, "initial limited request was not accepted: %s\n", first.c_str());
        rc = 10;
    }
    if (rc == 0) {
        const auto rejected = h1Exchange(endpoint, "GET /limited HTTP/1.1\r\nHost: x\r\n\r\n", elapsed);
        rc = checkDeadlineResponse("HTTP/1 rate-limit", rejected, elapsed);
    }

    server.stop();
    server.join();
    return rc;
}

[[nodiscard]] H2Result h2RejectingRequestOverSocket(const ruvia::detail::RouteTable& routes) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    H2Result result;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::test::Http2SansIoSessionFixture fixture;
            auto dispatcher = std::make_shared<WorkerDispatcher>(io, 64);
            const auto workerHandle = WorkerHandleAccess::make(dispatcher);
            auto sessionContext = fixture.context(ruvia::detail::ContextServices{}.withPlainTransport("127.0.0.1").withWorker(workerHandle));
            co_await taskAsAwaitable(runHttp2SansIoSession(sock, routes, worker, sessionContext));
        },
        asio::detached);

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

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/stream");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(headerBlock, "accept-encoding", "identity;q=0, gzip;q=0, br;q=0, zstd;q=0");
            const auto started = std::chrono::steady_clock::now();
            if (!co_await writeAll(frame(0x1 /*HEADERS*/, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            HpackDecoder decoder({.resource = std::pmr::get_default_resource()});
            while (!result.ended) {
                char headerBytes[kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    break;
                }
                if (header.streamId != 1) {
                    continue;
                }
                if (header.type == 0x1 /*HEADERS*/) {
                    (void)decoder.decode(std::string_view(payload.data(), payload.size()), &result, [](void* target, std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            static_cast<H2Result*>(target)->status.assign(value);
                        }
                        return true;
                    });
                } else if (header.type == 0x0 /*DATA*/) {
                    result.body.append(payload);
                }
                if ((header.flags & kHttp2FlagEndStream) != 0 && (header.type == 0x0 || header.type == 0x1)) {
                    result.ended = true;
                    result.elapsed = std::chrono::steady_clock::now() - started;
                }
            }
            asio::error_code ignored;
            sock.shutdown(tcp::socket::shutdown_both, ignored);
        },
        asio::detached);

    io.run();
    return result;
}

[[nodiscard]] int runHttp2NegotiationErrorDeadline() {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    const auto deadline = ruvia::detail::makeMiddlewareDescriptor<ruvia::Deadline<static_cast<std::int64_t>(kDeadline.count())>>();
    impl.registerResponseStreamRoute(
        ruvia::HttpKnownMethod::kGet,
        std::pmr::string("/stream", std::pmr::get_default_resource()),
        ruvia::detail::RouteStreamHandler(nullptr, &unusedStreamHandler),
        {},
        std::span(&deadline, std::size_t{1}));

    ruvia::HttpErrorHandler errorHandler(DeadlineAwareErrorHandler{});
    impl.setErrorHandler(ruvia::detail::CallbackAccess::ref(errorHandler));
    impl.finalize();

    const auto result = h2RejectingRequestOverSocket(impl.routeTable());
    if (!result.ended) {
        std::fputs("HTTP/2 negotiation error did not end the stream\n", stderr);
        return 20;
    }
    return checkDeadlineResponse("HTTP/2 negotiation", result.status, result.elapsed) == 0 ? 0 : 21;
}

}  // namespace

int main() {
    if (const auto rc = runHttp1RateLimitErrorDeadline(); rc != 0) {
        return rc;
    }
    if (const auto rc = runHttp2NegotiationErrorDeadline(); rc != 0) {
        return rc;
    }
    return 0;
}
