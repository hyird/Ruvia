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
#include "ruvia/web/detail/util/CallableRef.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/HttpServer.h"

namespace {

template <typename Handler>
void registerRoute(ruvia::detail::RouterImpl& router, ruvia::HttpKnownMethod method, std::string_view path, Handler& handler) {
    router.registerRoute(method, std::pmr::string(path, std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(handler), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
}

// Read one response head and return its start line (e.g. "HTTP/1.1 404 ...").
[[nodiscard]] std::string readResponseHead(asio::ip::tcp::socket& socket, std::error_code& ec) {
    asio::streambuf buffer;
    asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    return std::string(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + std::min<std::size_t>(buffer.size(), 15));
}

// Send one bodyless request, then a second on the same connection, and require
// both to be answered with the expected status -- i.e. the connection was kept
// alive across the first response.
[[nodiscard]] int expectKeepAlive(asio::io_context& ctx, const asio::ip::tcp::endpoint& endpoint, std::string_view firstRequest, std::string_view secondRequest, std::string_view status, int errBase) {
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
    auto handler = [](ruvia::Context& c) -> ruvia::Task<ruvia::HttpResponse> { co_return c.text("ok"); };
    auto identityForbiddenHandler = [](ruvia::Context& c) -> ruvia::Task<ruvia::HttpResponse> {
        auto response = c.text("identity is not an acceptable representation");
        response.header("Cache-Control", "no-transform");
        response.header("Content-Type", "image/png");
        co_return response;
    };
    auto noContentHandler = [](ruvia::Context& c) -> ruvia::Task<ruvia::HttpResponse> {
        c.status(ruvia::http_status::kNoContent);
        co_return c.body(nullptr);
    };
    registerRoute(routerImpl, ruvia::HttpKnownMethod::kGet, "/only", handler);
    registerRoute(routerImpl, ruvia::HttpKnownMethod::kGet, "/identity-forbidden", identityForbiddenHandler);
    registerRoute(routerImpl, ruvia::HttpKnownMethod::kGet, "/empty", noContentHandler);
    routerImpl.finalize();

    ruvia::detail::HttpServerOptions options;

    ruvia::detail::HttpServer server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routerImpl.routeTable(), {}, options);
    server.start();
    const auto endpoint = server.localEndpoint();
    asio::io_context ctx;

    // 404 (no such path) keeps a bodyless connection alive.
    if (const int rc = expectKeepAlive(ctx, endpoint, "GET /missing-one HTTP/1.1\r\nHost: localhost\r\n\r\n", "GET /missing-two HTTP/1.1\r\nHost: localhost\r\n\r\n", "HTTP/1.1 404", 1)) {
        std::fputs("bodyless 404 did not keep the connection alive\n", stderr);
        server.stop();
        server.join();
        return rc;
    }

    // 405 (path exists, wrong method) does the same.
    if (const int rc = expectKeepAlive(ctx, endpoint, "DELETE /only HTTP/1.1\r\nHost: localhost\r\n\r\n", "PUT /only HTTP/1.1\r\nHost: localhost\r\n\r\n", "HTTP/1.1 405", 4)) {
        std::fputs("bodyless 405 did not keep the connection alive\n", stderr);
        server.stop();
        server.join();
        return rc;
    }

    // An empty acceptable coding set must not reject a response that has no
    // content. The handler status is needed before the protocol can decide
    // whether 406 is meaningful, and the connection remains reusable.
    {
        asio::ip::tcp::socket sock(ctx);
        std::error_code ec;
        sock.connect(endpoint, ec);
        asio::write(sock,
            asio::buffer(std::string_view(
                "GET /empty HTTP/1.1\r\nHost: localhost\r\n"
                "Accept-Encoding: identity;q=0, gzip;q=0, br;q=0, zstd;q=0\r\n\r\n")),
            ec);
        if (!readResponseHead(sock, ec).starts_with("HTTP/1.1 204")) {
            std::fputs("bodyless response was rejected by empty coding negotiation\n", stderr);
            server.stop();
            server.join();
            return 10;
        }
        asio::write(sock, asio::buffer(std::string_view("GET /missing-after-empty HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
        if (ec || !readResponseHead(sock, ec).starts_with("HTTP/1.1 404")) {
            std::fputs("connection was not reusable after bodyless negotiated response\n", stderr);
            server.stop();
            server.join();
            return 11;
        }
    }

    // The request selects gzip but explicitly rejects identity. The route's
    // no-transform/incompressible metadata prevents compression, so the
    // framework must convert the buffered response to 406 before writing it;
    // silently sending the identity body would violate Accept-Encoding.
    {
        asio::ip::tcp::socket sock(ctx);
        std::error_code ec;
        sock.connect(endpoint, ec);
        asio::write(sock,
            asio::buffer(std::string_view(
                "GET /identity-forbidden HTTP/1.1\r\nHost: localhost\r\n"
                "Accept-Encoding: gzip, identity;q=0\r\n\r\n")),
            ec);
        if (!readResponseHead(sock, ec).starts_with("HTTP/1.1 406")) {
            std::fputs("forbidden identity fallback was not converted to 406\n", stderr);
            server.stop();
            server.join();
            return 9;
        }
    }

    // A not-found request that still owes body bytes must close the connection.
    {
        asio::ip::tcp::socket sock(ctx);
        std::error_code ec;
        sock.connect(endpoint, ec);
        asio::write(sock,
            asio::buffer(std::string_view("POST /missing HTTP/1.1\r\nHost: localhost\r\n"
                                          "Content-Length: 8\r\n\r\n")),
            ec);
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
