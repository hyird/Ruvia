// WebSocket upgrades do not select an HTTP response representation. A client
// may reject every response content coding with Accept-Encoding and still get a
// valid 101 upgrade; applying the buffered/streaming representation negotiation
// to the bodyless handshake incorrectly turns the upgrade into 406.

#include <cstdio>
#include <memory_resource>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

namespace {

ruvia::Task<void> closeAfterUpgrade(void*, ruvia::Context&) {
    co_return;
}

[[nodiscard]] std::string readHead(
    asio::ip::tcp::socket& socket, asio::streambuf& buffer, std::error_code& ec) {
    const std::size_t n = asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    std::string head(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + n);
    buffer.consume(n);
    return head;
}

}  // namespace

int main() {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
        std::pmr::string("/ws", std::pmr::get_default_resource()),
        ruvia::detail::RouteStreamHandler(nullptr, &closeAfterUpgrade), {}, {});
    impl.finalize();

    ruvia::detail::HttpServerOptions options;
    ruvia::detail::WebWorkerRuntime server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), impl.routeTable(), {},
        options);
    server.start();

    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    std::error_code ec;
    socket.connect(server.localEndpoint(), ec);
    if (ec) {
        std::fputs("client failed to connect to websocket test server\n", stderr);
        server.stop();
        server.join();
        return 1;
    }

    asio::write(socket,
        asio::buffer(
            std::string_view("GET /ws HTTP/1.1\r\n"
                             "Host: localhost\r\n"
                             "Connection: Upgrade\r\n"
                             "Upgrade: websocket\r\n"
                             "Sec-WebSocket-Version: 13\r\n"
                             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                             "Accept-Encoding: identity;q=0, gzip;q=0, br;q=0, zstd;q=0\r\n\r\n")),
        ec);
    if (ec) {
        std::fputs("client failed to write websocket handshake\n", stderr);
        server.stop();
        server.join();
        return 2;
    }

    asio::streambuf buffer;
    const auto head = readHead(socket, buffer, ec);
    std::error_code ignored;
    socket.close(ignored);
    server.stop();
    server.join();

    if (!head.starts_with("HTTP/1.1 101")) {
        std::fputs("websocket upgrade was rejected by Accept-Encoding negotiation\n", stderr);
        return 3;
    }
    if (head.find("Content-Encoding:") != std::string::npos ||
        head.find("content-encoding:") != std::string::npos) {
        std::fputs("websocket handshake carried response content coding metadata\n", stderr);
        return 4;
    }
    return 0;
}
