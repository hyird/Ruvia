// End-to-end proof of the edge node: a real EdgeServer in front of a loopback
// origin that counts how often it is hit. It checks that a first request is a
// proxied MISS, a repeat is served from cache as a HIT without touching the
// origin, a runtime purge forces the next request back to the origin, a
// non-GET method is rejected, and a runtime removeOrigin makes the mapping
// disappear -- exercising the dynamic add/remove-config and cache control the
// whole feature is about.

#include <atomic>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/edge/EdgeServer.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

using asio::ip::tcp;

// A loopback origin that replies with a fixed cacheable body and counts requests.
class OriginServer final {
public:
    OriginServer() : acceptor_(io_, tcp::endpoint(tcp::v4(), 0)) {}

    ~OriginServer() { stop(); }

    void start() {
        asio::co_spawn(io_, acceptLoop(), asio::detached);
        thread_ = std::jthread([this] { io_.run(); });
    }

    void stop() {
        io_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
    [[nodiscard]] int hits() const { return hits_.load(); }

private:
    asio::awaitable<void> acceptLoop() {
        for (;;) {
            auto [ec, socket] =
                co_await acceptor_.async_accept(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                break;
            }
            asio::co_spawn(io_, serve(std::move(socket)), asio::detached);
        }
    }

    asio::awaitable<void> serve(tcp::socket socket) {
        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos) {
            auto [ec, n] = co_await socket.async_read_some(
                asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
            if (n > 0) {
                request.append(buffer, n);
            }
            if (ec) {
                co_return;
            }
        }
        hits_.fetch_add(1);
        static constexpr std::string_view kResponse =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 5\r\n"
            "Cache-Control: max-age=60\r\n"
            "\r\n"
            "hello";
        co_await asio::async_write(
            socket, asio::buffer(kResponse), asio::as_tuple(asio::use_awaitable));
        asio::error_code ignore;
        socket.shutdown(tcp::socket::shutdown_both, ignore);
    }

    asio::io_context io_;
    tcp::acceptor acceptor_;
    std::atomic<int> hits_{0};
    std::jthread thread_;
};

// A synchronous one-shot HTTP/1.1 client: send `request`, read until the server
// closes, return the raw response bytes.
std::string httpRaw(std::uint16_t port, const std::string& request) {
    asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    asio::write(socket, asio::buffer(request));
    std::string response;
    char buffer[4096];
    asio::error_code ec;
    for (;;) {
        const std::size_t n = socket.read_some(asio::buffer(buffer), ec);
        if (n > 0) {
            response.append(buffer, n);
        }
        if (ec) {
            break;
        }
    }
    return response;
}

std::string httpGet(std::uint16_t port, std::string_view host, std::string_view target) {
    std::string request = "GET ";
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append(host);
    request.append("\r\nConnection: close\r\n\r\n");
    return httpRaw(port, request);
}

[[nodiscard]] int statusOf(const std::string& raw) {
    // "HTTP/1.1 " is 9 bytes; the status code is the next three digits.
    if (raw.size() < 12 || !raw.starts_with("HTTP/1.1 ")) {
        return -1;
    }
    return (raw[9] - '0') * 100 + (raw[10] - '0') * 10 + (raw[11] - '0');
}

[[nodiscard]] std::string bodyOf(const std::string& raw) {
    const auto pos = raw.find("\r\n\r\n");
    return pos == std::string::npos ? std::string{} : raw.substr(pos + 4);
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    using ruvia::edge::EdgeServer;
    using ruvia::edge::OriginSettings;

    OriginServer origin;
    origin.start();

    EdgeServer edge(tcp::endpoint(tcp::v4(), 0));
    edge.start();
    check(edge.addOrigin("front.local",
                         OriginSettings{"127.0.0.1", origin.port(), false}),
          "addOrigin maps the front host at runtime");
    const std::uint16_t edgePort = edge.localEndpoint().port();

    // First request: a proxied cache miss reaches the origin.
    {
        const auto r = httpGet(edgePort, "front.local", "/page");
        check(statusOf(r) == 200, "first request proxied with 200");
        check(bodyOf(r) == "hello", "origin body proxied to the client");
        check(contains(r, "X-Cache: MISS"), "first request is a cache MISS");
        check(origin.hits() == 1, "origin was contacted once");
    }

    // Second request: served from cache, the origin is not contacted again.
    {
        const auto r = httpGet(edgePort, "front.local", "/page");
        check(statusOf(r) == 200, "cached request still 200");
        check(bodyOf(r) == "hello", "cached body matches");
        check(contains(r, "X-Cache: HIT"), "second request is a cache HIT");
        check(contains(r, "Age: "), "cache hit carries an Age header");
        check(origin.hits() == 1, "cache hit did not contact the origin");
    }

    // A non-GET method is rejected before any origin work.
    {
        const auto r = httpRaw(edgePort,
                               "DELETE /page HTTP/1.1\r\nHost: front.local\r\n"
                               "Connection: close\r\n\r\n");
        check(statusOf(r) == 501, "non-GET method is rejected with 501");
        check(origin.hits() == 1, "rejected method did not contact the origin");
    }

    // Purging the entry forces the next request back to the origin.
    {
        check(edge.purge("front.local", "/page"), "purge reports a removed entry");
        const auto r = httpGet(edgePort, "front.local", "/page");
        check(contains(r, "X-Cache: MISS"), "post-purge request is a MISS");
        check(origin.hits() == 2, "post-purge request re-contacted the origin");
    }

    // Removing the origin mapping at runtime makes the host unroutable.
    {
        check(edge.removeOrigin("front.local"), "removeOrigin drops the mapping");
        const auto r = httpGet(edgePort, "front.local", "/page");
        check(statusOf(r) == 502, "unmapped host yields 502");
    }

    edge.stop();
    origin.stop();

    if (failures == 0) {
        std::fprintf(stderr, "edge server: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
