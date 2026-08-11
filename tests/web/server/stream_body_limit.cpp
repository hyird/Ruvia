#include <cstdio>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/web/BodyLimit.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/HttpServer.h"

namespace {

ruvia::Task<ruvia::HttpResponse> readStreamingBody(void*, ruvia::Context& context) {
    std::size_t bytes = 0;
    auto& reader = context.req().bodyReader();
    while (auto chunk = co_await reader.read()) {
        bytes += chunk->size();
    }

    std::pmr::string body(context.allocator<char>());
    body.append("accepted=");
    body.append(std::to_string(bytes));
    co_return context.text(std::move(body));
}

[[nodiscard]] std::string readHead(asio::ip::tcp::socket& socket, asio::streambuf& buffer, std::error_code& ec) {
    const auto bytes = asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    std::string head(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + bytes);
    buffer.consume(bytes);
    return head;
}

}  // namespace

int main() {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    const auto bodyLimit = ruvia::detail::makeMiddlewareDescriptor<ruvia::BodyLimit<8>>();
    impl.registerRoute(
        ruvia::HttpKnownMethod::kPost,
        std::pmr::string("/upload", std::pmr::get_default_resource()),
        ruvia::detail::RouteHandler(nullptr, &readStreamingBody),
        ruvia::detail::RequestBodyMode::kStream,
        std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
        std::span(&bodyLimit, std::size_t{1}));
    impl.finalize();

    ruvia::detail::HttpServerOptions options;
    // Leave stream bodies otherwise unlimited; this test specifically proves
    // the route-declared limit is carried into the streaming reader.
    options.maxStreamBodyBytes.reset();

    ruvia::detail::HttpServer server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), impl.routeTable(), {}, options);
    server.start();
    const auto endpoint = server.localEndpoint();

    int rc = 0;
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    std::error_code ec;
    socket.connect(endpoint, ec);
    if (ec) {
        std::fputs("connect failed\n", stderr);
        rc = 1;
    }

    if (rc == 0) {
        // 5 + 4 decoded bytes exceeds BodyLimit<8>. Content-Length is absent,
        // so the only place that can reject this is the streaming body reader.
        asio::write(socket, asio::buffer(std::string_view(
                                "POST /upload HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "5\r\nhello\r\n"
                                "4\r\nboom\r\n"
                                "0\r\n\r\n")),
            ec);
        if (ec) {
            std::fputs("request write failed\n", stderr);
            rc = 2;
        }
    }

    if (rc == 0) {
        asio::streambuf buffer;
        const auto head = readHead(socket, buffer, ec);
        if (!head.starts_with("HTTP/1.1 413")) {
            std::fputs("chunked stream route body exceeded BodyLimit but was not rejected with 413\n", stderr);
            rc = 3;
        }
    }

    std::error_code ignored;
    socket.close(ignored);
    server.stop();
    server.join();
    return rc;
}
