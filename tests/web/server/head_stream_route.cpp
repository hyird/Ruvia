// An explicit HEAD streaming/SSE route must answer like the corresponding GET
// route's head but without any body (RFC 9110 9.3.2). Streaming GET routes do
// not receive implicit HEAD shadows; this drives a real HTTP/1.1 loopback
// server and asserts explicit HEAD responses carry the streaming head (chunked
// framing / SSE content type), leak zero body bytes, and keep the connection
// alive for a pipelined GET that still streams normally.

#include <algorithm>
#include <charconv>
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

#include <zlib.h>

#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/HttpServer.h"

namespace {

ruvia::Task<void> tickStreamHandler(void*, ruvia::Context& context) {
    co_await context.stream().write("tick-1");
    co_await context.stream().write("tick-2");
    co_await context.stream().end();
}

ruvia::Task<void> noTransformStreamHandler(void*, ruvia::Context& context) {
    context.header("Cache-Control", "no-transform");
    context.header("Content-Type", "image/png");
    co_await context.stream().write("identity-forbidden-stream");
    co_await context.stream().end();
}

[[nodiscard]] std::string readHead(asio::ip::tcp::socket& socket, asio::streambuf& buffer, std::error_code& ec) {
    const std::size_t n = asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    std::string head(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + n);
    buffer.consume(n);
    return head;
}

[[nodiscard]] std::string lowered(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

[[nodiscard]] std::string decodeChunked(std::string_view wire) {
    std::string body;
    std::size_t offset = 0;
    for (;;) {
        const auto lineEnd = wire.find("\r\n", offset);
        if (lineEnd == std::string_view::npos) {
            return {};
        }
        std::size_t chunkSize = 0;
        const auto parsed = std::from_chars(wire.data() + offset, wire.data() + lineEnd, chunkSize, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != wire.data() + lineEnd) {
            return {};
        }
        offset = lineEnd + 2;
        if (chunkSize == 0) {
            return body;
        }
        if (chunkSize > wire.size() - offset || wire.size() - offset - chunkSize < 2 || wire.substr(offset + chunkSize, 2) != "\r\n") {
            return {};
        }
        body.append(wire.data() + offset, chunkSize);
        offset += chunkSize + 2;
    }
}

[[nodiscard]] std::string gzipDecode(std::string_view encoded) {
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 32) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(encoded.data()));
    stream.avail_in = static_cast<uInt>(encoded.size());
    std::string decoded;
    char buffer[1024];
    for (;;) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        const auto status = inflate(&stream, Z_NO_FLUSH);
        decoded.append(buffer, sizeof(buffer) - stream.avail_out);
        if (status == Z_STREAM_END) {
            (void)inflateEnd(&stream);
            return decoded;
        }
        if (status != Z_OK || (stream.avail_in == 0 && stream.avail_out != 0)) {
            (void)inflateEnd(&stream);
            return {};
        }
    }
}

}  // namespace

int main() {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    std::pmr::string eventsPath("/events", std::pmr::get_default_resource());
    std::pmr::string ssePath("/sse", std::pmr::get_default_resource());
    std::pmr::string noTransformPath("/no-transform", std::pmr::get_default_resource());
    impl.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string(eventsPath, std::pmr::get_default_resource()), ruvia::detail::RouteStreamHandler(nullptr, &tickStreamHandler), {}, {});
    impl.registerResponseStreamRoute(ruvia::HttpKnownMethod::kHead, std::move(eventsPath), ruvia::detail::RouteStreamHandler(nullptr, &tickStreamHandler), {}, {});
    impl.registerSseRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string(ssePath, std::pmr::get_default_resource()), ruvia::detail::RouteStreamHandler(nullptr, &tickStreamHandler), {}, {});
    impl.registerSseRoute(ruvia::HttpKnownMethod::kHead, std::move(ssePath), ruvia::detail::RouteStreamHandler(nullptr, &tickStreamHandler), {}, {});
    impl.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet, std::move(noTransformPath), ruvia::detail::RouteStreamHandler(nullptr, &noTransformStreamHandler), {}, {});
    impl.finalize();

    ruvia::detail::HttpServerOptions options;
    ruvia::detail::HttpServer server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), impl.routeTable(), {}, options);
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

    // Explicit HEAD of the generic streaming route: the streaming head, no body bytes.
    asio::write(sock, asio::buffer(std::string_view("HEAD /events HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n\r\n")), ec);
    const auto eventsHead = lowered(readHead(sock, buffer, ec));
    if (!eventsHead.starts_with("http/1.1 200")) {
        fail(1, "explicit HEAD of a streaming route was not 200");
    } else if (buffer.size() != 0) {
        fail(2, "explicit HEAD of a streaming route sent body bytes");
    } else if (eventsHead.find("connection: close") != std::string_view::npos) {
        fail(3, "explicit HEAD of a streaming route forced the connection closed");
    } else if (eventsHead.find("content-encoding: gzip") == std::string_view::npos) {
        fail(4, "explicit HEAD of a streaming route lost the negotiated content coding");
    }

    // Explicit HEAD of the SSE route mirrors the GET head, content type included.
    if (rc == 0) {
        asio::write(sock, asio::buffer(std::string_view("HEAD /sse HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n\r\n")), ec);
        const auto sseHead = lowered(readHead(sock, buffer, ec));
        if (!sseHead.starts_with("http/1.1 200")) {
            fail(5, "explicit HEAD of an SSE route was not 200");
        } else if (sseHead.find("content-type: text/event-stream") == std::string_view::npos) {
            fail(6, "explicit HEAD of an SSE route lost the SSE content type");
        } else if (buffer.size() != 0) {
            fail(7, "explicit HEAD of an SSE route sent body bytes");
        } else if (sseHead.find("content-encoding: gzip") == std::string_view::npos) {
            fail(8, "explicit HEAD of an SSE route lost the negotiated content coding");
        }
    }

    // The connection stayed alive and correctly framed: a pipelined GET on the
    // same socket must still stream the full chunked body.
    if (rc == 0) {
        asio::write(sock, asio::buffer(std::string_view("GET /events HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n\r\n")), ec);
        const auto getHead = lowered(readHead(sock, buffer, ec));
        if (!getHead.starts_with("http/1.1 200")) {
            fail(9, "GET after HEAD did not parse as a clean 200 response");
        } else if (getHead.find("transfer-encoding: chunked") == std::string_view::npos) {
            fail(10, "streaming GET after HEAD was not chunked");
        } else if (getHead.find("content-encoding: gzip") == std::string_view::npos) {
            fail(11, "streaming GET did not advertise gzip");
        } else {
            asio::read_until(sock, buffer, "0\r\n\r\n", ec);
            const std::string wireBody(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + buffer.size());
            const auto encoded = decodeChunked(wireBody);
            if (gzipDecode(encoded) != "tick-1tick-2") {
                fail(12, "streaming GET body after HEAD was not a valid gzip stream");
            }
        }
    }

    // A request that forbids identity and every supported coding is rejected
    // before route dispatch; it must not silently stream an identity body.
    if (rc == 0) {
        asio::ip::tcp::socket rejectedSock(ctx);
        rejectedSock.connect(endpoint, ec);
        asio::write(rejectedSock, asio::buffer(std::string_view("GET /events HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: identity;q=0, gzip;q=0, br;q=0, zstd;q=0\r\n\r\n")), ec);
        asio::streambuf rejectedBuffer;
        const auto rejectedHead = lowered(readHead(rejectedSock, rejectedBuffer, ec));
        if (!rejectedHead.starts_with("http/1.1 406")) {
            fail(13, "an empty response coding set was not rejected with 406");
        }
        rejectedSock.close(ec);
    }

    // Compression is negotiated, but the streaming head explicitly forbids
    // transformation. The sink must reject before committing a 200 head;
    // after commitment there is no legal way to turn the response into 406.
    if (rc == 0) {
        asio::ip::tcp::socket rejectedSock(ctx);
        rejectedSock.connect(endpoint, ec);
        asio::write(rejectedSock, asio::buffer(std::string_view(
            "GET /no-transform HTTP/1.1\r\nHost: localhost\r\n"
            "Accept-Encoding: gzip, identity;q=0\r\n\r\n")), ec);
        asio::streambuf rejectedBuffer;
        const auto rejectedHead = lowered(readHead(rejectedSock, rejectedBuffer, ec));
        if (!rejectedHead.starts_with("http/1.1 406")) {
            fail(14, "streaming identity fallback was not rejected before commit");
        }
        rejectedSock.close(ec);
    }

    sock.close(ec);
    server.stop();
    server.join();
    return rc;
}
