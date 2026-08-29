// Regression: an idle keep-alive connection (one request served, no bytes of the
// next request received yet) must be governed by idleTimeout -- nginx
// keepalive_timeout semantics -- not requestHeaderTimeout. Previously the idle
// wait for the next request ran under kReadingInitial (requestHeaderTimeout), so a
// connection with a long idleTimeout was still dropped one requestHeaderTimeout
// after the previous response.
//
// The server uses a short requestHeaderTimeout and a long idleTimeout. After
// one request the client idles and watches the socket: the connection must NOT be
// closed within a window that is well past requestHeaderTimeout but far under
// idleTimeout. The read is driven by a safety timer so a close is observed
// as an eof rather than a hang. (The connection's FIRST request is still bounded
// by requestHeaderTimeout -- see server_max_connections.)

#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <string_view>
#include <system_error>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

using namespace std::chrono_literals;

namespace {

// Short header deadline, long keepalive deadline; the observation window sits
// between them so the two outcomes are unambiguous.
constexpr auto kRequestHeaderTimeout = 200ms;
constexpr auto kIdleTimeout = 3s;
constexpr auto kObserveWindow = 700ms;

}  // namespace

int main() {
    std::pmr::memory_resource* resource = std::pmr::get_default_resource();
    ruvia::detail::RouteTable routes(resource);  // empty -> 404, keep-alive

    ruvia::detail::HttpServerOptions options;
    options.requestHeaderTimeout = kRequestHeaderTimeout;
    options.idleTimeout = kIdleTimeout;
    options.scanInterval = 50ms;

    ruvia::detail::WebWorkerRuntime server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routes, {},
        std::move(options));
    server.start();
    const auto endpoint = server.localEndpoint();

    int result = 0;
    {
        asio::io_context ctx;
        asio::ip::tcp::socket socket(ctx);
        std::error_code ec;
        socket.connect(endpoint, ec);
        if (ec) {
            std::fputs("connect failed\n", stderr);
            result = 1;
        }

        // Serve one request so the connection enters keep-alive idle.
        if (result == 0) {
            asio::write(socket,
                asio::buffer(std::string_view("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
            char resp[2048];
            const auto got = socket.read_some(asio::buffer(resp), ec);
            if (ec || got == 0) {
                std::fputs("first request failed\n", stderr);
                result = 2;
            }
        }

        // Idle past requestHeaderTimeout, well under idleTimeout. A close in
        // this window means the idle wait is (wrongly) bounded by requestHeaderTimeout.
        if (result == 0) {
            bool closed = false;
            char buf[64];
            asio::steady_timer safety(ctx, kObserveWindow);
            safety.async_wait([&](const std::error_code& e) {
                if (!e) {
                    socket.cancel();
                }
            });
            socket.async_read_some(asio::buffer(buf), [&](const std::error_code& e, std::size_t) {
                if (e && e != asio::error::operation_aborted) {
                    closed = true;  // server closed us (eof/reset)
                }
                safety.cancel();
            });
            ctx.run();
            if (closed) {
                std::fputs(
                    "idle keep-alive connection was closed within the observation "
                    "window: the idle wait is bounded by requestHeaderTimeout, not "
                    "idleTimeout\n",
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
