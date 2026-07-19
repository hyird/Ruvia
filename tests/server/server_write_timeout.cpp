// Regression: sendTimeout must bound the response-write phase (kWriting), so a
// slow-read client -- one that sends a request then stops reading -- cannot pin a
// connection (and its response buffer) open indefinitely. A handler returns a
// body far larger than the socket buffers; the client sends the request, stops
// reading past sendTimeout, then drains. The server must have closed the stuck
// write, observed as an eof after the buffered prefix rather than a stall.

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/socket_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include "ruvia/web/Context.h"
#include "ruvia/web/Router.h"
#include "ruvia/web/detail/CallableRef.h"
#include "ruvia/web/detail/router/RouterInternal.h"
#include "ruvia/web/detail/server/HttpServer.h"

using namespace std::chrono_literals;

namespace {
constexpr auto kSendTimeout = 200ms;
// Past sendTimeout, so a stuck write is already closed when the client drains.
constexpr auto kStallBeforeDrain = 700ms;
constexpr int kClientReceiveBufferBytes = 1024;
// Keep the advertised receive window explicitly small so loopback auto-tuning
// cannot absorb the whole response before the write timeout starts measuring.
std::string bigBody(4 * 1024 * 1024, 'x');
char drainBuf[65536];
}  // namespace

int main() {
    ruvia::Router router;
    auto& routerImpl = ruvia::detail::RouterImpl::from(router);
    auto handler = [](ruvia::Context& c) -> ruvia::Task<ruvia::HttpResponse> {
        co_return c.text(std::string_view(bigBody));
    };
    routerImpl.registerRoute(
        ruvia::HttpKnownMethod::kGet,
        std::pmr::string("/big", std::pmr::get_default_resource()),
        ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(handler),
        ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.finalize();

    ruvia::detail::HttpServerOptions options;
    options.sendTimeout = kSendTimeout;
    options.scanInterval = 50ms;
    options.shutdownGracePeriod = 0ms;

    ruvia::detail::HttpServer server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
        routerImpl.routeTable(), {}, std::move(options));
    server.start();
    const auto endpoint = server.localEndpoint();

    int result = 0;
    {
        asio::io_context ctx;
        asio::ip::tcp::socket socket(ctx);
        std::error_code ec;
        socket.open(endpoint.protocol(), ec);
        if (ec) {
            std::fputs("socket open failed\n", stderr);
            result = 1;
        }

        if (result == 0) {
            socket.set_option(
                asio::socket_base::receive_buffer_size(kClientReceiveBufferBytes),
                ec);
            if (ec) {
                std::fputs("receive buffer setup failed\n", stderr);
                result = 1;
            }
        }

        if (result == 0) {
            socket.connect(endpoint, ec);
            if (ec) {
                std::fputs("connect failed\n", stderr);
                result = 1;
            }
        }

        if (result == 0) {
            asio::write(socket, asio::buffer(std::string_view(
                "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
            if (ec) {
                std::fputs("request write failed\n", stderr);
                result = 2;
            }
        }

        if (result == 0) {
            // Do not read yet: let the write stall long enough for sendTimeout.
            std::this_thread::sleep_for(kStallBeforeDrain);

            // Drain: a stuck write that sendTimeout closed yields eof after the
            // buffered prefix; an unbounded write would keep the socket open and
            // the read would stall until the safety timer.
            bool closed = false;
            asio::steady_timer safety(ctx, 2000ms);
            safety.async_wait([&](const std::error_code& e) { if (!e) socket.cancel(); });
            std::function<void()> readMore;
            readMore = [&]() {
                socket.async_read_some(asio::buffer(drainBuf),
                    [&](const std::error_code& e, std::size_t) {
                        if (e) {
                            if (e != asio::error::operation_aborted) closed = true;
                            safety.cancel();
                            return;
                        }
                        readMore();
                    });
            };
            readMore();
            ctx.run();
            if (!closed) {
                std::fputs(
                    "response write to a non-reading client was not closed by "
                    "sendTimeout: slow-read connections can be held open\n",
                    stderr);
                result = 3;
            }
        }

        std::error_code ignored;
        socket.close(ignored);
    }

    server.stop();
    server.join();
    return result;
}
