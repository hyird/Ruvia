// A HEAD request for a document-root file must answer exactly like GET but
// without the body (RFC 9110 9.3.2): the same 200 status and Content-Length,
// and zero body bytes on the wire. The document-root fallback used to accept
// only GET, so HEAD of a file that answered 200 to GET returned 404 -- breaking
// caches, health checks, and link checkers that probe with HEAD. Drives a real
// HTTP/1.1 loopback server with a document root and asserts the HEAD framing is
// byte-correct by pipelining a follow-up GET that must still parse cleanly.

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

#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServer.h"

namespace {

constexpr std::string_view kFileBody = "hello-static-file";  // 17 bytes

// Read one response head (up to and including the blank line) and drop it from
// the buffer, leaving any following bytes (a body, or a pipelined response) for
// the caller to inspect.
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
    ruvia::StaticRoot root(dir, {});

    std::pmr::memory_resource* resource = std::pmr::get_default_resource();
    ruvia::detail::RouteTable routes(resource);
    ruvia::detail::HttpServerOptions options;
    options.documentRoot.root = &root;
    options.shutdownGracePeriod = std::chrono::milliseconds(0);

    ruvia::detail::HttpServer server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
        routes, {}, options);
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
    asio::write(sock, asio::buffer(std::string_view(
        "HEAD /asset.txt HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
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
        asio::write(sock, asio::buffer(std::string_view(
            "GET /asset.txt HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
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
                    asio::buffer_copy(
                        asio::buffer(body.data(), take), buffer.data());
                    buffer.consume(take);
                }
                if (have < length) {
                    asio::read(
                        sock, asio::buffer(body.data() + have, length - have), ec);
                }
                if (body != kFileBody) {
                    fail(6, "GET body after HEAD did not match the file");
                }
            }
        }
    }

    sock.close(ec);
    server.stop();
    server.join();
    fs::remove_all(dir);
    return rc;
}
