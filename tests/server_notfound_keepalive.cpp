// A 404 for an unmatched route must keep a bodyless HTTP/1.1 connection alive
// (so clients hitting missing paths can reuse it), while a not-found request
// that still owes body bytes must close the connection -- the server never
// consumes that body, so keeping the connection open would desync the next
// request. Both are checked against a server with no routes at all.

#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <string>
#include <string_view>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServer.h"

namespace {

// Read one response head and return its start line (e.g. "HTTP/1.1 404 ...").
[[nodiscard]] std::string readResponseHead(
    asio::ip::tcp::socket& socket, std::error_code& ec) {
    asio::streambuf buffer;
    asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    std::string head(
        asio::buffers_begin(buffer.data()),
        asio::buffers_begin(buffer.data()) +
            std::min<std::size_t>(buffer.size(), 15));
    return head;
}

}  // namespace

int main() {
    std::pmr::memory_resource* resource = std::pmr::get_default_resource();
    ruvia::detail::RouteTable routes(resource);
    ruvia::detail::HttpServerOptions options;
    options.shutdownGracePeriod = std::chrono::milliseconds(0);

    ruvia::detail::HttpServer server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
        routes, {}, options);
    server.start();
    const auto endpoint = server.localEndpoint();
    asio::io_context ctx;

    // Case 1: two bodyless GET 404s must both be answered on one connection.
    {
        asio::ip::tcp::socket sock(ctx);
        std::error_code ec;
        sock.connect(endpoint, ec);

        asio::write(sock, asio::buffer(std::string_view(
            "GET /missing-one HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
        const auto head1 = readResponseHead(sock, ec);
        if (ec || head1.rfind("HTTP/1.1 404", 0) != 0) {
            std::fputs("first 404 not returned\n", stderr);
            return 1;
        }

        asio::write(sock, asio::buffer(std::string_view(
            "GET /missing-two HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
        if (ec) {
            std::fputs("connection closed after a bodyless 404 (no keep-alive)\n", stderr);
            return 2;
        }
        const auto head2 = readResponseHead(sock, ec);
        if (ec || head2.rfind("HTTP/1.1 404", 0) != 0) {
            std::fputs("second 404 not served on the kept-alive connection\n", stderr);
            return 3;
        }
    }

    // Case 2: a not-found POST that still owes a body must close the connection,
    // since the server does not drain that body.
    {
        asio::ip::tcp::socket sock(ctx);
        std::error_code ec;
        sock.connect(endpoint, ec);

        // Send the head with a declared body but withhold the body bytes, so the
        // request "owes" content the not-found path will never consume.
        asio::write(sock, asio::buffer(std::string_view(
            "POST /missing HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Length: 8\r\n\r\n")), ec);
        const auto head = readResponseHead(sock, ec);
        if (ec || head.rfind("HTTP/1.1 404", 0) != 0) {
            std::fputs("not-found POST did not get a 404\n", stderr);
            return 4;
        }
        // The connection must be closed: a follow-up read sees end-of-stream.
        char byte = 0;
        (void)asio::read(sock, asio::buffer(&byte, 1), ec);
        if (ec != asio::error::eof && ec != asio::error::connection_reset) {
            std::fputs("bodied not-found request did not close the connection\n", stderr);
            return 5;
        }
    }

    server.stop();
    server.join();
    return 0;
}
