// A HEAD request for a streaming or SSE GET route must answer like GET but
// without any body (RFC 9110 9.3.2): the auto HEAD shadow used to cover only
// buffered GET routes, so HEAD of an SSE endpoint answered 404 -- and once the
// shadow exists, the handler must stop deterministically at its first write
// instead of streaming forever into a body-suppressed response. Drives a real
// HTTP/1.1 loopback server and asserts the HEAD responses carry the streaming
// head (chunked framing / SSE content type), leak zero body bytes, and keep
// the connection alive for a pipelined GET that still streams normally.

#include <algorithm>
#include <cctype>
#include <chrono>
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
#include "ruvia/web/Router.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/detail/router/RouterInternal.h"
#include "ruvia/web/detail/server/HttpServer.h"

namespace {

ruvia::Task<void> tickStreamHandler(void*, ruvia::Context& context) {
    co_await context.stream().write("tick-1");
    co_await context.stream().write("tick-2");
    co_await context.stream().end();
}

[[nodiscard]] std::string readHead(
    asio::ip::tcp::socket& socket, asio::streambuf& buffer, std::error_code& ec) {
    const std::size_t n = asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    std::string head(
        asio::buffers_begin(buffer.data()),
        asio::buffers_begin(buffer.data()) + n);
    buffer.consume(n);
    return head;
}

[[nodiscard]] std::string lowered(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

}  // namespace

int main() {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    std::pmr::string eventsPath("/events", std::pmr::get_default_resource());
    std::pmr::string ssePath("/sse", std::pmr::get_default_resource());
    impl.registerResponseStreamRoute(
        ruvia::HttpKnownMethod::kGet, std::move(eventsPath),
        ruvia::detail::RouteStreamHandler(nullptr, &tickStreamHandler), {}, {});
    impl.registerSseRoute(
        ruvia::HttpKnownMethod::kGet, std::move(ssePath),
        ruvia::detail::RouteStreamHandler(nullptr, &tickStreamHandler), {}, {});
    impl.finalize();

    ruvia::detail::HttpServerOptions options;
    options.shutdownGracePeriod = std::chrono::milliseconds(0);
    ruvia::detail::HttpServer server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
        impl.routeTable(), {}, options);
    server.start();
    const auto endpoint = server.localEndpoint();

    int rc = 0;
    auto fail = [&](int code, const char* message) {
        std::fputs(message, stderr);
        std::fputc('\n', stderr);
        rc = code;
    };

    asio::io_context ctx;
    asio::ip::tcp::socket sock(ctx);
    std::error_code ec;
    sock.connect(endpoint, ec);
    asio::streambuf buffer;

    // HEAD of the generic streaming route: the streaming head, no body bytes.
    asio::write(sock, asio::buffer(std::string_view(
        "HEAD /events HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
    const auto eventsHead = lowered(readHead(sock, buffer, ec));
    if (!eventsHead.starts_with("http/1.1 200")) {
        fail(1, "HEAD of a streaming route was not 200");
    } else if (buffer.size() != 0) {
        fail(2, "HEAD of a streaming route sent body bytes");
    } else if (eventsHead.contains("connection: close")) {
        fail(3, "HEAD of a streaming route forced the connection closed");
    }

    // HEAD of the SSE route mirrors the GET head, content type included.
    if (rc == 0) {
        asio::write(sock, asio::buffer(std::string_view(
            "HEAD /sse HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
        const auto sseHead = lowered(readHead(sock, buffer, ec));
        if (!sseHead.starts_with("http/1.1 200")) {
            fail(4, "HEAD of an SSE route was not 200");
        } else if (!sseHead.contains("content-type: text/event-stream")) {
            fail(5, "HEAD of an SSE route lost the SSE content type");
        } else if (buffer.size() != 0) {
            fail(6, "HEAD of an SSE route sent body bytes");
        }
    }

    // The connection stayed alive and correctly framed: a pipelined GET on the
    // same socket must still stream the full chunked body.
    if (rc == 0) {
        asio::write(sock, asio::buffer(std::string_view(
            "GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
        const auto getHead = lowered(readHead(sock, buffer, ec));
        if (!getHead.starts_with("http/1.1 200")) {
            fail(7, "GET after HEAD did not parse as a clean 200 response");
        } else if (!getHead.contains("transfer-encoding: chunked")) {
            fail(8, "streaming GET after HEAD was not chunked");
        } else {
            asio::read_until(sock, buffer, "0\r\n\r\n", ec);
            const std::string body(
                asio::buffers_begin(buffer.data()),
                asio::buffers_begin(buffer.data()) + buffer.size());
            if (!body.contains("tick-1") ||
                !body.contains("tick-2")) {
                fail(9, "streaming GET body after HEAD was incomplete");
            }
        }
    }

    sock.close(ec);
    server.stop();
    server.join();
    return rc;
}
