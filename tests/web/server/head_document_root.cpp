// A HEAD request for a document-root file must answer exactly like GET but
// without the body (RFC 9110 9.3.2): the same 200 status and Content-Length,
// and zero body bytes on the wire. The document-root fallback used to accept
// only GET, so HEAD of a file that answered 200 to GET returned 404 -- breaking
// caches, health checks, and link checkers that probe with HEAD. Drives a real
// HTTP/1.1 loopback server with a document root and asserts the HEAD framing is
// byte-correct by pipelining a follow-up GET that must still parse cleanly.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <zlib.h>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Router.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServer.h"

namespace {

const std::string kFileBody(4096, 'a');

// Read one response head (up to and including the blank line) and drop it from
// the buffer, leaving any following bytes (a body, or a pipelined response) for
// the caller to inspect.
[[nodiscard]] std::string readHead(asio::ip::tcp::socket& socket, asio::streambuf& buffer, std::error_code& ec) {
    const std::size_t n = asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    std::string head(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + n);
    buffer.consume(n);
    return head;
}

[[nodiscard]] std::size_t contentLength(std::string_view head) {
    for (std::string_view rest = head; !rest.empty();) {
        const auto eol = rest.find("\r\n");
        const auto line = rest.substr(0, eol);
        constexpr std::string_view name = "content-length:";
        if (line.size() > name.size()) {
            bool match = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(line[i])) != name[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::size_t value = 0;
                for (char c : line.substr(name.size())) {
                    if (c >= '0' && c <= '9') {
                        value = value * 10 + static_cast<std::size_t>(c - '0');
                    }
                }
                return value;
            }
        }
        if (eol == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(eol + 2);
    }
    return std::string_view::npos;
}

[[nodiscard]] std::string gzipDecode(std::string_view encoded) {
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 32) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(encoded.data()));
    stream.avail_in = static_cast<uInt>(encoded.size());
    std::string decoded;
    char buffer[4096];
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

[[nodiscard]] std::string gzipEncode(std::string_view plain) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    std::string encoded(compressBound(static_cast<uLong>(plain.size())) + 64, '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(plain.data()));
    stream.avail_in = static_cast<uInt>(plain.size());
    stream.next_out = reinterpret_cast<Bytef*>(encoded.data());
    stream.avail_out = static_cast<uInt>(encoded.size());
    const int status = deflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END) {
        (void)deflateEnd(&stream);
        return {};
    }
    encoded.resize(stream.total_out);
    (void)deflateEnd(&stream);
    return encoded;
}

ruvia::Task<ruvia::HttpResponse> staticFileRoute(void* target, ruvia::Context& context) {
    co_return context.staticFile(*static_cast<ruvia::StaticRoot*>(target), "dynamic.txt", "text/plain");
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_head_docroot_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "asset.txt");
        f << kFileBody;
    }
    {
        std::ofstream f(dir / "dynamic.txt");
        f << kFileBody;
    }
    const auto sidecarBody = gzipEncode(kFileBody);
    {
        std::ofstream f(dir / "asset.txt.gz", std::ios::binary);
        f.write(sidecarBody.data(), static_cast<std::streamsize>(sidecarBody.size()));
    }
    ruvia::StaticRoot root(dir, {});

    std::pmr::memory_resource* resource = std::pmr::get_default_resource();
    ruvia::Router router;
    auto& routerImpl = ruvia::detail::RouterImpl::from(router);
    routerImpl.registerRoute(
        ruvia::HttpKnownMethod::kGet,
        std::pmr::string("/handler-static", resource),
        ruvia::detail::RouteHandler(&root, &staticFileRoute),
        ruvia::detail::RequestBodyMode::kBuffered,
        {},
        {});
    routerImpl.finalize();
    ruvia::BlockingPool pool(ruvia::BlockingPoolOptions{.threadCount = 1});
    ruvia::detail::HttpServerOptions options;
    options.documentRoot.root = &root;
    options.blockingPool = &pool;
    options.compression->minBytes = 1;

    ruvia::detail::HttpServer server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routerImpl.routeTable(), {}, options);
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

    // HEAD of the file: 200, Content-Length of the whole representation, no body.
    asio::write(sock, asio::buffer(std::string_view("HEAD /asset.txt HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
    const std::string headHead = readHead(sock, buffer, ec);
    if (!headHead.starts_with("HTTP/1.1 200")) {
        fail(1, "HEAD of a document-root file was not 200");
    } else if (contentLength(headHead) != kFileBody.size()) {
        fail(2, "HEAD Content-Length did not describe the full representation");
    } else if (buffer.size() != 0) {
        // Any bytes buffered past the HEAD header block are a leaked body.
        fail(3, "HEAD sent body bytes it must not send");
    }

    // Pipeline a GET on the same connection. If HEAD had leaked a body, these
    // bytes would be misframed and this head would not start with a status line.
    if (rc == 0) {
        asio::write(sock, asio::buffer(std::string_view("GET /asset.txt HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
        const std::string getHead = readHead(sock, buffer, ec);
        if (!getHead.starts_with("HTTP/1.1 200")) {
            fail(4, "GET after HEAD did not parse as a clean 200 response");
        } else {
            const std::size_t length = contentLength(getHead);
            if (length != kFileBody.size()) {
                fail(5, "GET Content-Length mismatch after HEAD");
            } else {
                std::string body(length, '\0');
                std::size_t have = buffer.size();
                if (have > 0) {
                    const std::size_t take = std::min(have, length);
                    asio::buffer_copy(asio::buffer(body.data(), take), buffer.data());
                    buffer.consume(take);
                }
                if (have < length) {
                    asio::read(sock, asio::buffer(body.data() + have, length - have), ec);
                }
                if (body != kFileBody) {
                    fail(6, "GET body after HEAD did not match the file");
                }
            }
        }
    }

    // When identity is explicitly forbidden, an unindexed file must still be
    // served if the configured runtime can compress it. The document-root
    // fallback defers the strict representation decision until this blocking
    // compression step completes.
    if (rc == 0) {
        asio::write(sock, asio::buffer(std::string_view(
            "GET /asset.txt HTTP/1.1\r\nHost: localhost\r\n"
            "Accept-Encoding: gzip, identity;q=0\r\n\r\n")), ec);
        const std::string compressedHead = readHead(sock, buffer, ec);
        if (!compressedHead.starts_with("HTTP/1.1 200")) {
            fail(7, "deferred static compression did not return 200");
        } else if (compressedHead.find("Content-Encoding: gzip") == std::string::npos) {
            fail(8, "deferred static compression did not advertise gzip");
        } else {
            const std::size_t length = contentLength(compressedHead);
            if (length == std::string_view::npos || length >= kFileBody.size()) {
                fail(9, "deferred static compression did not reduce the file");
            } else {
                std::string encoded(length, '\0');
                const std::size_t have = std::min(buffer.size(), length);
                if (have != 0) {
                    asio::buffer_copy(asio::buffer(encoded.data(), have), buffer.data());
                    buffer.consume(have);
                }
                if (have < length) {
                    asio::read(sock, asio::buffer(encoded.data() + have, length - have), ec);
                }
                if (ec || gzipDecode(encoded) != kFileBody) {
                    fail(10, "deferred static compression body was not valid gzip");
                }
            }
        }
    }

    // A handler's Context::staticFile uses the same deferred policy as the
    // document-root fallback. This file has no sidecar, so a successful 200
    // proves the final buffered-response stage, not sidecar selection.
    if (rc == 0) {
        asio::write(sock, asio::buffer(std::string_view(
            "GET /handler-static HTTP/1.1\r\nHost: localhost\r\n"
            "Accept-Encoding: gzip, identity;q=0\r\n\r\n")), ec);
        const std::string routeHead = readHead(sock, buffer, ec);
        if (!routeHead.starts_with("HTTP/1.1 200")) {
            fail(11, "Context::staticFile route was not compressed successfully");
        } else if (routeHead.find("Content-Encoding: gzip") == std::string::npos) {
            fail(12, "Context::staticFile route did not advertise gzip");
        } else {
            const std::size_t length = contentLength(routeHead);
            std::string encoded(length == std::string_view::npos ? 0 : length, '\0');
            const std::size_t have = std::min(buffer.size(), encoded.size());
            if (have != 0) {
                asio::buffer_copy(asio::buffer(encoded.data(), have), buffer.data());
                buffer.consume(have);
            }
            if (have < encoded.size()) {
                asio::read(sock, asio::buffer(encoded.data() + have, encoded.size() - have), ec);
            }
            if (ec || gzipDecode(encoded) != kFileBody) {
                fail(13, "Context::staticFile route body was not valid gzip");
            }
        }
    }

    sock.close(ec);
    server.stop();
    server.join();

    // A precompressed sidecar is a complete representation in its own right;
    // it must remain usable when runtime compression is disabled. Before the
    // negotiation/capability split this request was rejected at head parsing,
    // before the document-root index could select asset.txt.gz.
    options.compression.reset();
    ruvia::detail::HttpServer sidecarServer(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routerImpl.routeTable(), {}, options);
    sidecarServer.start();
    asio::ip::tcp::socket sidecarSocket(ctx);
    sidecarSocket.connect(sidecarServer.localEndpoint(), ec);
    asio::streambuf sidecarBuffer;
    asio::write(sidecarSocket, asio::buffer(std::string_view(
        "GET /asset.txt HTTP/1.1\r\nHost: localhost\r\n"
        "Accept-Encoding: gzip, identity;q=0\r\n\r\n")), ec);
    const std::string sidecarHead = readHead(sidecarSocket, sidecarBuffer, ec);
    if (!sidecarHead.starts_with("HTTP/1.1 200")) {
        fail(14, "a precompressed sidecar was rejected when runtime compression was disabled");
    } else if (sidecarHead.find("Content-Encoding: gzip") == std::string::npos) {
        fail(15, "the static sidecar response lost Content-Encoding: gzip");
    } else {
        const std::size_t length = contentLength(sidecarHead);
        std::string encoded(length == std::string_view::npos ? 0 : length, '\0');
        const std::size_t have = std::min(sidecarBuffer.size(), encoded.size());
        if (have != 0) {
            asio::buffer_copy(asio::buffer(encoded.data(), have), sidecarBuffer.data());
            sidecarBuffer.consume(have);
        }
        if (have < encoded.size()) {
            asio::read(sidecarSocket, asio::buffer(encoded.data() + have, encoded.size() - have), ec);
        }
        if (ec || gzipDecode(encoded) != kFileBody) {
            fail(16, "the static sidecar response body was not the indexed gzip representation");
        }
    }
    sidecarSocket.close(ec);
    sidecarServer.stop();
    sidecarServer.join();
    fs::remove_all(dir);
    return rc;
}
