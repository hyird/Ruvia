// A failure past the point of no return has nowhere to go through onError: the
// response head is already on the wire, so the status cannot be changed and the
// session can only drop the connection. That used to end the exception too --
// the operator saw a truncated response and nothing else. The connection-failure
// sink is where the reason survives the connection.
//
// Drives a real HTTP/1.1 loopback server whose streaming route throws after
// committing its head, and asserts the listener sees exactly that exception,
// that the peer address comes with it, and that the server keeps serving.

#include <chrono>
#include <cstdio>
#include <exception>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/ServerConfig.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

namespace {

// Commits the head, writes one chunk, then fails. Everything before the throw
// has already reached the client, which is what makes this unanswerable.
ruvia::Task<void> failingStreamHandler(void*, ruvia::Context& context) {
    co_await context.stream().write("partial");
    throw std::runtime_error("handler failed mid-stream");
}

ruvia::Task<void> healthyStreamHandler(void*, ruvia::Context& context) {
    co_await context.stream().write("ok");
    co_await context.stream().end();
}

struct FailureObservation final {
    std::size_t calls{0};
    std::string message;
    std::string remoteAddress;

    void operator()(const ruvia::ConnectionFailureRecord& record) noexcept {
        ++calls;
        try {
            remoteAddress.assign(record.remoteAddress());
            std::rethrow_exception(record.exception());
        } catch (const std::exception& error) {
            message.assign(error.what());
        } catch (...) {
            message.assign("<unknown>");
        }
    }
};

[[nodiscard]] std::string readHead(asio::ip::tcp::socket& socket, asio::streambuf& buffer, std::error_code& ec) {
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
    std::pmr::string failingPath("/boom", std::pmr::get_default_resource());
    std::pmr::string healthyPath("/fine", std::pmr::get_default_resource());
    impl.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet, std::move(failingPath), ruvia::detail::RouteStreamHandler(nullptr, &failingStreamHandler), {}, {});
    impl.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet, std::move(healthyPath), ruvia::detail::RouteStreamHandler(nullptr, &healthyStreamHandler), {}, {});
    impl.finalize();

    FailureObservation observation;
    ruvia::detail::HttpServerOptions options;
    options.connectionFailure.callback = ruvia::detail::CallbackAccess::bind<void(const ruvia::ConnectionFailureRecord&) noexcept>(observation);
    ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), impl.routeTable(), {}, options);
    server.start();
    const auto endpoint = server.localEndpoint();

    int rc = 0;
    auto fail = [&](int code, const char* message) {
        std::fputs(message, stderr);
        std::fputc('\n', stderr);
        rc = code;
    };

    asio::io_context ctx;
    std::error_code ec;

    // The committed stream that then throws: the client sees a truncated
    // response and the listener sees why.
    {
        asio::ip::tcp::socket sock(ctx);
        sock.connect(endpoint, ec);
        asio::streambuf buffer;
        asio::write(sock, asio::buffer(std::string_view("GET /boom HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
        const auto head = readHead(sock, buffer, ec);
        if (!head.starts_with("HTTP/1.1 200")) {
            fail(1, "the failing stream route did not commit its head first");
        }
        // Drain until the peer closes; the session drops the connection.
        asio::streambuf rest;
        std::error_code readEc;
        asio::read_until(sock, rest, "never-appears", readEc);
        sock.close(ec);
    }

    // The server survived the failed connection and still serves the next one.
    if (rc == 0) {
        asio::ip::tcp::socket sock(ctx);
        sock.connect(endpoint, ec);
        asio::streambuf buffer;
        asio::write(sock, asio::buffer(std::string_view("GET /fine HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
        const auto head = readHead(sock, buffer, ec);
        if (!head.starts_with("HTTP/1.1 200")) {
            fail(2, "the server stopped serving after a connection failure");
        }
        asio::read_until(sock, buffer, "0\r\n\r\n", ec);
        sock.close(ec);
    }

    server.stop();
    server.join();

    if (rc == 0) {
        if (observation.calls != 1) {
            fail(3, "the connection failure did not reach the listener exactly once");
        } else if (observation.message != "handler failed mid-stream") {
            fail(4, "the listener did not receive the original exception");
        } else if (observation.remoteAddress.empty()) {
            fail(5, "the failure did not carry the peer address");
        }
    }

    // The same failure is counted, so a server with no listener installed is
    // still observable, and every connection released its slot on the way out.
    if (rc == 0) {
        const auto stats = server.stats();
        if (stats.connectionFailures != 1) {
            fail(6, "the connection failure was not counted");
        } else if (stats.activeConnections != 0) {
            fail(7, "a finished connection kept its slot");
        } else if (stats.workerFailures != 0) {
            fail(8, "a single connection failure was escalated to the worker");
        }
    }
    return rc;
}
