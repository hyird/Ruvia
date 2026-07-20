// A response for a request that matched no handler -- a 404 (no such path) or a
// 405 (path exists, wrong method) -- must keep a bodyless HTTP/1.1 connection
// alive (so clients probing missing paths or methods can reuse it), matching the
// matched-route and static-file paths. A not-found request that still owes body
// bytes must instead close, since the server never drains that body and keeping
// the connection open would desync the next request.

#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <string>
#include <string_view>
#include <thread>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/web/Context.h"
#include "ruvia/web/Router.h"
#include "ruvia/web/detail/CallableRef.h"
#include "ruvia/web/detail/router/RouterInternal.h"
#include "ruvia/web/detail/server/HttpServer.h"

namespace {

template <typename Handler>
void registerRoute(
    ruvia::detail::RouterImpl& router,
    ruvia::HttpKnownMethod method,
    std::string_view path,
    Handler& handler) {
    router.registerRoute(
        method,
        std::pmr::string(path, std::pmr::get_default_resource()),
        ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(
            handler),
        ruvia::detail::RequestBodyMode::kBuffered,
        {},
        {});
}

// Read one response head and return its start line (e.g. "HTTP/1.1 404 ...").
[[nodiscard]] std::string readResponseHead(
    asio::ip::tcp::socket& socket, std::error_code& ec) {
    asio::streambuf buffer;
    asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    return std::string(
        asio::buffers_begin(buffer.data()),
        asio::buffers_begin(buffer.data()) +
            std::min<std::size_t>(buffer.size(), 15));
}

// Send one bodyless request, then a second on the same connection, and require
// both to be answered with the expected status -- i.e. the connection was kept
// alive across the first response.
[[nodiscard]] int expectKeepAlive(
    asio::io_context& ctx,
    const asio::ip::tcp::endpoint& endpoint,
    std::string_view firstRequest,
    std::string_view secondRequest,
    std::string_view status,
    int errBase) {
    asio::ip::tcp::socket sock(ctx);
    std::error_code ec;
    sock.connect(endpoint, ec);

    asio::write(sock, asio::buffer(firstRequest), ec);
    if (!readResponseHead(sock, ec).starts_with(status)) {
        return errBase;
    }
    asio::write(sock, asio::buffer(secondRequest), ec);
    if (ec) {
        return errBase + 1;  // connection closed -> no keep-alive
    }
    if (!readResponseHead(sock, ec).starts_with(status)) {
        return errBase + 2;
    }
    return 0;
}

}  // namespace

int main() {
    ruvia::Router router;
    auto& routerImpl = ruvia::detail::RouterImpl::from(router);
    auto handler = [](ruvia::Context& c) -> ruvia::Task<ruvia::HttpResponse> {
        co_return c.text("ok");
    };
    registerRoute(routerImpl, ruvia::HttpKnownMethod::kGet, "/only", handler);
    routerImpl.finalize();

    ruvia::detail::HttpServerOptions options;
    options.shutdownGracePeriod = std::chrono::milliseconds(0);

    ruvia::detail::HttpServer server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
        routerImpl.routeTable(), {}, options);
    server.start();
    const auto endpoint = server.localEndpoint();
    asio::io_context ctx;

    // 404 (no such path) keeps a bodyless connection alive.
    if (const int rc = expectKeepAlive(
            ctx, endpoint,
            "GET /missing-one HTTP/1.1\r\nHost: localhost\r\n\r\n",
            "GET /missing-two HTTP/1.1\r\nHost: localhost\r\n\r\n",
            "HTTP/1.1 404", 1)) {
        std::fputs("bodyless 404 did not keep the connection alive\n", stderr);
        server.stop();
        server.join();
        return rc;
    }

    // 405 (path exists, wrong method) does the same.
    if (const int rc = expectKeepAlive(
            ctx, endpoint,
            "DELETE /only HTTP/1.1\r\nHost: localhost\r\n\r\n",
            "PUT /only HTTP/1.1\r\nHost: localhost\r\n\r\n",
            "HTTP/1.1 405", 4)) {
        std::fputs("bodyless 405 did not keep the connection alive\n", stderr);
        server.stop();
        server.join();
        return rc;
    }

    // A not-found request that still owes body bytes must close the connection.
    {
        asio::ip::tcp::socket sock(ctx);
        std::error_code ec;
        sock.connect(endpoint, ec);
        asio::write(sock, asio::buffer(std::string_view(
            "POST /missing HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Length: 8\r\n\r\n")), ec);
        if (!readResponseHead(sock, ec).starts_with("HTTP/1.1 404")) {
            std::fputs("bodied not-found did not get a 404\n", stderr);
            server.stop();
            server.join();
            return 7;
        }
        char byte = 0;
        (void)asio::read(sock, asio::buffer(&byte, 1), ec);
        if (ec != asio::error::eof && ec != asio::error::connection_reset) {
            std::fputs("bodied not-found did not close the connection\n", stderr);
            server.stop();
            server.join();
            return 8;
        }
    }

    server.stop();
    server.join();
    return 0;
}
