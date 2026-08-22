// Protocol errors may be answered by the user error handler. If that handler
// returns a static file while precompressed static-file negotiation is enabled,
// the HTTP/1 session must not dereference a response-coding selection that was
// never negotiated because parsing failed before a complete request head
// existed. No request-time file compression is involved.

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/web/Context.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

namespace {

constexpr std::string_view kErrorBody = "file-backed error response\n";

[[nodiscard]] std::string readHead(asio::ip::tcp::socket& socket, asio::streambuf& buffer, std::error_code& ec) {
    const std::size_t n = asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    std::string head(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + n);
    buffer.consume(n);
    return head;
}

struct FileErrorHandler final {
    ruvia::StaticRoot* root{nullptr};

    ruvia::Task<ruvia::HttpResponse> operator()(ruvia::Context& context, ruvia::HttpErrorInfo info) const {
        context.status(info.status());
        co_return context.staticFile(*root, {.relativePath = "error.txt", .contentType = "text/plain"});
    }
};

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("ruvia_error_static_file_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "error.txt");
        f << kErrorBody;
    }

    ruvia::StaticRoot root(dir, {});
    ruvia::HttpErrorHandler errorHandler(FileErrorHandler{.root = &root});

    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.setErrorHandler(ruvia::detail::CallbackAccess::ref(errorHandler));
    impl.finalize();

    ruvia::detail::HttpServerOptions options;
    options.documentRoot = ruvia::detail::HttpServerOptions::DocumentRoot::standalone(root);
    options.compression.emplace();

    ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), impl.routeTable(), {}, options);
    server.start();

    int rc = 0;
    auto fail = [&](int code, const char* message) {
        std::fputs(message, stderr);
        std::fputc('\n', stderr);
        rc = code;
    };

    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    std::error_code ec;
    socket.connect(server.localEndpoint(ruvia::ListenerId{1}), ec);
    if (ec) {
        fail(1, "client failed to connect");
    }

    if (rc == 0) {
        // Exceed the request-header limit before the blank line arrives, so
        // parsing fails before a complete request head and no response
        // content-coding policy has been negotiated.
        std::string request =
            "GET / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "X-Fill: ";
        request.append(70 * 1024, 'a');
        asio::write(socket, asio::buffer(request), ec);
        if (ec) {
            fail(2, "client failed to write invalid request");
        }
    }

    asio::streambuf buffer;
    std::string head;
    if (rc == 0) {
        head = readHead(socket, buffer, ec);
        if (!head.starts_with("HTTP/1.1 431")) {
            fail(3, "file-backed header-limit error did not return HTTP/1.1 431");
        }
    }

    std::error_code ignored;
    socket.close(ignored);
    server.stop();
    server.join();
    fs::remove_all(dir, ignored);

    if (rc == 0 && server.stats().connectionFailures != 0) {
        fail(4, "file-backed parse error escaped as a connection failure");
    }
    return rc;
}
