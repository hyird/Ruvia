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
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

namespace {

ruvia::Task<ruvia::HttpResponse> readBufferedBody(void*, ruvia::Context& context) {
    const auto body = co_await context.req().text();
    co_return context.body(body);
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
        std::pmr::string("/echo", std::pmr::get_default_resource()),
        ruvia::detail::RouteHandler(nullptr, &readBufferedBody),
        ruvia::detail::RequestBodyMode::kBuffered,
        std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
        std::span(&bodyLimit, std::size_t{1}));
    impl.finalize();

    ruvia::detail::HttpServerOptions options;

    ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), impl.routeTable(), {}, options);
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
        // so the lazy buffered body reader must enforce the route limit.
        asio::write(socket, asio::buffer(std::string_view(
                                "POST /echo HTTP/1.1\r\n"
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
            std::fputs("chunked buffered route body exceeded BodyLimit but was not rejected with 413\n", stderr);
            rc = 3;
        }
    }

    std::error_code ignored;
    socket.close(ignored);
    server.stop();
    server.join();
    return rc;
}
