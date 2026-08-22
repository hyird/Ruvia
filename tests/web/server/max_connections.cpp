// The per-worker maxConnections cap (bounded by default) must reject a
// connection accepted past the limit, and must free the slot again once a
// counted connection ends -- a leaked count would wedge the acceptor.
//
// A connection is held "counted" by sending only a partial request head, so the
// session stays open waiting out requestHeaderTimeout. An over-limit connection
// is instead closed immediately (well under that timeout); timing the close
// distinguishes rejection from an admitted connection merely awaiting a request.

#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <string_view>
#include <thread>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

namespace {

constexpr auto kHeaderTimeout = std::chrono::seconds(4);
// Rejection closes the socket before protocol detection, so it lands far under
// the header timeout; this bound separates the two outcomes with wide margin.
constexpr auto kRejectionBound = std::chrono::seconds(1);

void connectAndHold(asio::ip::tcp::socket& socket, const asio::ip::tcp::endpoint& endpoint) {
    std::error_code ec;
    socket.connect(endpoint, ec);
    // Partial head: no terminating CRLF CRLF, so the server keeps the session
    // open (and counted) waiting for the rest.
    asio::write(socket, asio::buffer(std::string_view("GET / HTTP/1.1\r\n")), ec);
}

}  // namespace

int main() {
    std::pmr::memory_resource* resource = std::pmr::get_default_resource();
    ruvia::detail::RouteTable routes(resource);

    ruvia::detail::HttpServerOptions options;
    options.maxConnections = 1;
    options.requestHeaderTimeout = kHeaderTimeout;

    ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routes, {}, options);
    server.start();
    const auto endpoint = server.localEndpoint(ruvia::ListenerId{1});

    asio::io_context clientContext;
    int result = 0;

    {
        // Fill the single slot.
        asio::ip::tcp::socket first(clientContext);
        connectAndHold(first, endpoint);
        // Let the accept loop admit and count the first connection before the
        // second races in (generous for a loaded CI runner).
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Over-limit connection must be closed promptly, not held for a request.
        asio::ip::tcp::socket second(clientContext);
        std::error_code ec;
        second.connect(endpoint, ec);
        const auto begin = std::chrono::steady_clock::now();
        char byte = 0;
        (void)asio::read(second, asio::buffer(&byte, 1), ec);
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        if (ec != asio::error::eof && ec != asio::error::connection_reset) {
            std::fputs("over-limit connection was not closed\n", stderr);
            result = 1;
        } else if (elapsed >= kRejectionBound) {
            std::fputs("over-limit connection was admitted, not rejected\n", stderr);
            result = 2;
        }

        // Release the counted slot.
        std::error_code ignored;
        first.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        first.close(ignored);
    }

    if (result == 0) {
        // The freed slot must let a fresh connection be served -- a leaked count
        // would keep rejecting.
        bool served = false;
        for (int attempt = 0; attempt < 150 && !served; ++attempt) {
            asio::ip::tcp::socket third(clientContext);
            std::error_code ec;
            third.connect(endpoint, ec);
            if (!ec) {
                asio::write(third, asio::buffer(std::string_view("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
                asio::streambuf response;
                if (!ec) {
                    asio::read_until(third, response, "\r\n\r\n", ec);
                }
                served = !ec && response.size() > 0;
            }
            std::error_code ignored;
            third.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
            third.close(ignored);
            if (!served) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (!served) {
            std::fputs("slot was not freed after the counted connection ended\n", stderr);
            result = 3;
        }
    }

    if (result == 0) {
        // Shedding is a load-management decision the operator has to be able to
        // see: without a counter it is indistinguishable from a client hangup.
        const auto stats = server.stats();
        if (stats.connectionsRefused == 0) {
            std::fputs("the shed connection was not counted\n", stderr);
            result = 4;
        } else if (stats.connectionFailures != 0) {
            std::fputs("shedding a connection was counted as a failure\n", stderr);
            result = 5;
        }
    }

    server.stop();
    server.join();
    return result;
}
