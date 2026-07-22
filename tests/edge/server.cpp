// End-to-end proof of the edge node: a real EdgeServer in front of a loopback
// origin that counts how often it is hit. It checks that a first request is a
// proxied MISS, a repeat is served from cache as a HIT without touching the
// origin, a runtime purge forces the next request back to the origin, an unsafe
// method is proxied and invalidates the corresponding GET, and a runtime
// removeOrigin makes the mapping disappear -- exercising the dynamic
// add/remove-config and cache control the whole feature is about.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "tls_fixture.h"
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
        thread_ = std::thread([this] { io_.run(); });
    }

    void stop() {
        io_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        // Close the listener so later connects are refused promptly (the
        // stale-if-error test relies on a fast connection failure).
        asio::error_code ignore;
        acceptor_.close(ignore);
    }

    [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
    [[nodiscard]] int hits() const { return hits_.load(); }              // full 200s served
    [[nodiscard]] int notModified() const { return notModified_.load(); }  // 304s served
    [[nodiscard]] int slowRevalidations() const {
        return slowRevalidations_.load();
    }
    [[nodiscard]] std::string lastRequest() {
        std::lock_guard<std::mutex> guard(mutex_);
        return lastRequest_;
    }

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
        // Read any Content-Length body so a forwarded request is captured whole.
        const std::size_t headEnd = request.find("\r\n\r\n") + 4;
        std::size_t contentLength = 0;
        if (const auto p = request.find("Content-Length: "); p != std::string::npos) {
            std::size_t i = p + 16;
            while (i < request.size() && request[i] >= '0' && request[i] <= '9') {
                contentLength = contentLength * 10 + static_cast<std::size_t>(request[i] - '0');
                ++i;
            }
        }
        while (request.size() - headEnd < contentLength) {
            auto [ec, n] = co_await socket.async_read_some(
                asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
            if (n > 0) {
                request.append(buffer, n);
            }
            if (ec) {
                break;
            }
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            lastRequest_ = request;
        }
        // Respond conditionally: a matching If-None-Match yields a bodyless 304;
        // otherwise a full 200 with an ETag, short-lived (max-age=1) for /rev so
        // it can be driven stale, and long-lived (max-age=60) elsewhere.
        std::string response;
        if (request.find("GET /nostore-transition ") != std::string::npos) {
            const int phase = noStoreTransitions_.fetch_add(1);
            hits_.fetch_add(1);
            if (phase == 0) {
                response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Length: 5\r\n"
                    "ETag: \"v1\"\r\n"
                    "Cache-Control: max-age=1, stale-if-error=300\r\n"
                    "\r\n"
                    "hello";
            } else if (phase == 1) {
                response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Length: 5\r\n"
                    "Cache-Control: no-store\r\n"
                    "\r\n"
                    "newer";
            } else {
                response =
                    "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Length: 0\r\n"
                    "\r\n";
            }
        } else if (request.find("GET /swrslow ") != std::string::npos &&
            request.find("If-None-Match: \"v1\"") != std::string::npos) {
            // Keep a background refresh suspended long enough to stop the edge
            // while this detached operation still owns cache/config leases.
            slowRevalidations_.fetch_add(1);
            asio::steady_timer delay(io_);
            delay.expires_after(std::chrono::seconds(5));
            auto [ec] = co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                co_return;
            }
            notModified_.fetch_add(1);
            response =
                "HTTP/1.1 304 Not Modified\r\n"
                "ETag: \"v1\"\r\n"
                "Cache-Control: max-age=60\r\n"
                "\r\n";
        } else if (request.find("If-None-Match: \"v1\"") != std::string::npos) {
            // Revalidation refreshes the entry with a longer, stable freshness.
            notModified_.fetch_add(1);
            response =
                "HTTP/1.1 304 Not Modified\r\n"
                "ETag: \"v1\"\r\n"
                "Cache-Control: max-age=60\r\n"
                "\r\n";
        } else if (request.find("GET /chunked ") != std::string::npos ||
                   request.find("GET /chunked10 ") != std::string::npos) {
            // An unknown-length (chunked) origin response.
            hits_.fetch_add(1);
            response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Cache-Control: max-age=60\r\n"
                "\r\n"
                "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        } else if (request.find("GET /bigchunk ") != std::string::npos) {
            // A chunked body far larger than the HTTP/2 flow-control window
            // (65535 bytes), so streaming it exercises window exhaustion, the
            // deferred-suffix drain, and WINDOW_UPDATE-driven resumption.
            hits_.fetch_add(1);
            response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Cache-Control: max-age=60\r\n"
                "\r\n";
            for (int i = 0; i < 40; ++i) {  // 40 x 5000 = 200000 bytes
                response += "1388\r\n";     // 0x1388 == 5000
                response += std::string(5000, 'A');
                response += "\r\n";
            }
            response += "0\r\n\r\n";
        } else if (request.find("GET /vary ") != std::string::npos ||
                   request.find("GET /vary-q ") != std::string::npos ||
                   request.find("GET /vary-repeat ") != std::string::npos ||
                   request.find("GET /vary-empty ") != std::string::npos) {
            // Varies on Accept-Encoding: the edge caches a variant per encoding.
            hits_.fetch_add(1);
            response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 5\r\n"
                "Vary: Accept-Encoding\r\n"
                "Cache-Control: max-age=60\r\n"
                "\r\n"
                "hello";
        } else if (request.find("GET /varycookie ") != std::string::npos) {
            // Varies on a field the edge does not key on: must not be cached.
            hits_.fetch_add(1);
            response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 5\r\n"
                "Vary: Cookie\r\n"
                "Cache-Control: max-age=60\r\n"
                "\r\n"
                "hello";
        } else if (request.find("GET /hop ") != std::string::npos) {
            // Both faces nominate a custom hop-by-hop field through Connection.
            // The proxy must consume the nomination and not cache/forward it.
            hits_.fetch_add(1);
            response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 5\r\n"
                "Connection: close, X-Origin-Hop\r\n"
                "X-Origin-Hop: origin-secret\r\n"
                "X-End-To-End: retained\r\n"
                "Cache-Control: max-age=60\r\n"
                "\r\n"
                "hello";
        } else if (request.find("GET /aged ") != std::string::npos) {
            hits_.fetch_add(1);
            response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 5\r\n"
                "Age: 40\r\n"
                "Cache-Control: max-age=100\r\n"
                "\r\n"
                "hello";
        } else if (request.find("GET /swr ") != std::string::npos ||
                   request.find("GET /swrslow ") != std::string::npos) {
            // Short freshness with a stale-while-revalidate window and a validator.
            hits_.fetch_add(1);
            response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 5\r\n"
                "ETag: \"v1\"\r\n"
                "Cache-Control: max-age=1, stale-while-revalidate=30\r\n"
                "\r\n"
                "hello";
        } else if (request.find("GET /slow ") != std::string::npos) {
            // A slow response, so concurrent requests overlap in flight.
            hits_.fetch_add(1);
            asio::steady_timer delay(io_);
            delay.expires_after(std::chrono::milliseconds(300));
            co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
            response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 5\r\n"
                "Cache-Control: max-age=60\r\n"
                "\r\n"
                "hello";
        } else {
            hits_.fetch_add(1);
            const bool shortLived = request.find("GET /rev ") != std::string::npos ||
                request.find("GET /sie ") != std::string::npos;
            const bool staleIfError = request.find("GET /sie ") != std::string::npos;
            response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 5\r\n"
                "ETag: \"v1\"\r\n"
                "Cache-Control: max-age=";
            response += shortLived ? "1" : "60";
            if (staleIfError) {
                response += ", stale-if-error=300";
            }
            response += "\r\n\r\nhello";
        }
        co_await asio::async_write(
            socket, asio::buffer(response), asio::as_tuple(asio::use_awaitable));
        asio::error_code ignore;
        socket.shutdown(tcp::socket::shutdown_both, ignore);
    }

    asio::io_context io_;
    tcp::acceptor acceptor_;
    std::atomic<int> hits_{0};
    std::atomic<int> notModified_{0};
    std::atomic<int> slowRevalidations_{0};
    std::atomic<int> noStoreTransitions_{0};
    std::mutex mutex_;
    std::string lastRequest_;
    std::thread thread_;
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
    // A header worth forwarding, and one the edge should strip for MVP caching.
    request.append("\r\nUser-Agent: probe-agent");
    request.append("\r\nAccept-Encoding: gzip");
    request.append("\r\nConnection: close\r\n\r\n");
    return httpRaw(port, request);
}

std::string httpHead(std::uint16_t port, std::string_view host, std::string_view target) {
    std::string request = "HEAD ";
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append(host);
    // Same Accept-Encoding as httpGet so HEAD shares the GET's cache variant.
    request.append("\r\nAccept-Encoding: gzip");
    request.append("\r\nConnection: close\r\n\r\n");
    return httpRaw(port, request);
}

// GET with a Range header (same Accept-Encoding as httpGet, so it hits that variant).
std::string httpGetRange(
    std::uint16_t port,
    std::string_view host,
    std::string_view target,
    std::string_view range) {
    std::string request = "GET ";
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append(host);
    request.append("\r\nAccept-Encoding: gzip\r\nRange: ");
    request.append(range);
    request.append("\r\nConnection: close\r\n\r\n");
    return httpRaw(port, request);
}

// GET with an explicit Accept-Encoding, to exercise variant caching.
std::string httpGetEnc(
    std::uint16_t port,
    std::string_view host,
    std::string_view target,
    std::string_view acceptEncoding) {
    std::string request = "GET ";
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append(host);
    request.append("\r\nAccept-Encoding: ");
    request.append(acceptEncoding);
    request.append("\r\nConnection: close\r\n\r\n");
    return httpRaw(port, request);
}

std::string httpPost(
    std::uint16_t port,
    std::string_view host,
    std::string_view target,
    std::string_view body) {
    std::string request = "POST ";
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append(host);
    request.append("\r\nContent-Type: text/plain\r\nContent-Length: ");
    request.append(std::to_string(body.size()));
    request.append("\r\nConnection: close\r\n\r\n");
    request.append(body);
    return httpRaw(port, request);
}

// Read exactly one HTTP/1.1 response (headers plus its Content-Length body) from
// a connection that stays open, so a second request can follow.
std::string readOneResponse(asio::ip::tcp::socket& socket) {
    std::string response;
    char buffer[4096];
    asio::error_code ec;
    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const std::size_t n = socket.read_some(asio::buffer(buffer), ec);
        if (n > 0) {
            response.append(buffer, n);
        }
        headerEnd = response.find("\r\n\r\n");
        if (ec) {
            return response;
        }
    }
    const std::size_t bodyStart = headerEnd + 4;
    std::size_t contentLength = 0;
    if (const auto p = response.find("Content-Length: "); p != std::string::npos && p < headerEnd) {
        std::size_t i = p + 16;
        while (i < response.size() && response[i] >= '0' && response[i] <= '9') {
            contentLength = contentLength * 10 + static_cast<std::size_t>(response[i] - '0');
            ++i;
        }
    }
    while (response.size() - bodyStart < contentLength) {
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

// Send two GET requests on a single persistent connection (the first keep-alive,
// the second closing) and return both raw responses.
std::pair<std::string, std::string> httpKeepAliveTwo(
    std::uint16_t port,
    std::string_view host,
    std::string_view target,
    std::string_view version = "HTTP/1.1",
    std::string_view firstConnection = "keep-alive") {
    asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

    const auto send = [&](std::string_view connection) {
        std::string request = "GET ";
        request.append(target);
        request.push_back(' ');
        request.append(version);
        request.append("\r\nHost: ");
        request.append(host);
        request.append("\r\nConnection: ");
        request.append(connection);
        request.append("\r\n\r\n");
        asio::write(socket, asio::buffer(request));
    };

    send(firstConnection);
    std::string first = readOneResponse(socket);
    send("close");
    std::string second = readOneResponse(socket);
    return {first, second};
}

// A synchronous one-shot HTTPS client: TLS-handshake (no verification), send one
// GET, read until the server closes, return the raw response bytes.
std::string httpsGet(std::uint16_t port, std::string_view host, std::string_view target) {
    asio::io_context io;
    asio::ssl::context ctx(asio::ssl::context::tls_client);
    ctx.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<tcp::socket> stream(io, ctx);
    stream.lowest_layer().connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    stream.handshake(asio::ssl::stream_base::client);

    std::string request = "GET ";
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append(host);
    request.append("\r\nConnection: close\r\n\r\n");
    asio::write(stream, asio::buffer(request));

    std::string response;
    char buffer[4096];
    asio::error_code ec;
    for (;;) {
        const std::size_t n = stream.read_some(asio::buffer(buffer), ec);
        if (n > 0) {
            response.append(buffer, n);
        }
        if (ec) {
            break;
        }
    }
    return response;
}

// Run a shell command and capture its stdout (used to drive curl as an h2 client).
#if !defined(_WIN32)
std::string runShell(const std::string& command) {
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return output;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output.append(buffer);
    }
    pclose(pipe);
    return output;
}
#endif

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

[[nodiscard]] std::optional<std::uint64_t> ageOf(const std::string& raw) {
    constexpr std::string_view marker = "\r\nAge: ";
    const auto found = raw.find(marker);
    if (found == std::string::npos) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    std::size_t cursor = found + marker.size();
    const std::size_t begin = cursor;
    while (cursor < raw.size() && raw[cursor] >= '0' && raw[cursor] <= '9') {
        value = value * 10 + static_cast<std::uint64_t>(raw[cursor] - '0');
        ++cursor;
    }
    if (cursor == begin || raw.substr(cursor, 2) != "\r\n") {
        return std::nullopt;
    }
    return value;
}

// Decode an HTTP/1 chunked body (size lines in hex, CRLF-delimited, 0-terminated).
[[nodiscard]] std::string dechunk(const std::string& framed) {
    std::string out;
    std::size_t pos = 0;
    for (;;) {
        const auto crlf = framed.find("\r\n", pos);
        if (crlf == std::string::npos) {
            break;
        }
        const std::size_t size =
            static_cast<std::size_t>(std::stoul(framed.substr(pos, crlf - pos), nullptr, 16));
        if (size == 0) {
            break;
        }
        pos = crlf + 2;
        out.append(framed, pos, size);
        pos += size + 2;  // data plus trailing CRLF
    }
    return out;
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

    ruvia::edge::EdgeServerOptions options;
    EdgeServer edge(ruvia::edge::EdgeEndpoint{"0.0.0.0", 0}, options);
    edge.start();
    check(edge.addOrigin("front.local",
                         OriginSettings{"127.0.0.1", origin.port(), false}),
          "addOrigin maps the front host at runtime");
    const std::uint16_t edgePort = edge.localEndpoint().port;

    // First request: a proxied cache miss reaches the origin.
    {
        const auto r = httpGet(edgePort, "front.local", "/page");
        check(statusOf(r) == 200, "first request proxied with 200");
        check(bodyOf(r) == "hello", "origin body proxied to the client");
        check(contains(r, "X-Cache: MISS"), "first request is a cache MISS");
        check(origin.hits() == 1, "origin was contacted once");

        // The origin sees a curated, forwarded request.
        const auto seen = origin.lastRequest();
        check(contains(seen, "User-Agent: probe-agent"),
              "a forwardable client header reaches the origin");
        check(contains(seen, "Accept-Encoding: gzip"),
              "Accept-Encoding is forwarded so the origin may compress");
        check(contains(seen, "X-Forwarded-For: 127.0.0.1"),
              "X-Forwarded-For carries the client address");
        check(contains(seen, "X-Forwarded-Host: front.local"),
              "X-Forwarded-Host carries the front host");
        check(contains(seen, "X-Forwarded-Proto: http"),
              "X-Forwarded-Proto is set");
        check(contains(seen, "Via: 1.1 ruvia-edge"), "a Via header is added");
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

    // URI host comparison is case-insensitive, and equivalent spellings share
    // one cache key rather than fragmenting the cache.
    {
        const int before = origin.hits();
        const auto upper = httpGet(edgePort, "FRONT.LOCAL", "/host-case");
        check(statusOf(upper) == 200 && contains(upper, "X-Cache: MISS"),
              "uppercase Host resolves the configured origin");
        const auto lower = httpGet(edgePort, "front.local", "/host-case");
        check(contains(lower, "X-Cache: HIT"),
              "host case variants share a cache entry");
        check(origin.hits() == before + 1,
              "equivalent host case reaches the origin only once");
    }

    // A cache hit reports the origin's corrected Age plus resident time; it
    // must not restart the shared-cache age clock at zero.
    {
        const auto first = httpGet(edgePort, "front.local", "/aged");
        check(contains(first, "X-Cache: MISS"), "aged response is initially a MISS");
        const auto second = httpGet(edgePort, "front.local", "/aged");
        const auto age = ageOf(second);
        check(contains(second, "X-Cache: HIT"), "aged response is cached");
        check(age && *age >= 40,
              "cache hit preserves upstream Age instead of resetting it");
    }

    // Routing intentionally matches a front host independent of its optional
    // port, but the target URI authority remains part of the cache key. Purge
    // still invalidates every authority variant owned by that front-host route.
    {
        const int before = origin.hits();
        const auto port80 = httpGet(
            edgePort, "front.local:80", "/authority-port");
        const auto port81 = httpGet(
            edgePort, "front.local:81", "/authority-port");
        check(contains(port80, "X-Cache: MISS") &&
                  contains(port81, "X-Cache: MISS"),
              "different Host ports do not collide in the cache");
        check(origin.hits() == before + 2,
              "different target authorities each reach the origin");
        check(contains(origin.lastRequest(), "X-Forwarded-Host: front.local:81"),
              "X-Forwarded-Host preserves the request authority port");
        const auto port80Hit = httpGet(
            edgePort, "FRONT.LOCAL:80", "/authority-port");
        check(contains(port80Hit, "X-Cache: HIT"),
              "authority host case remains canonicalized");

        check(edge.purge("front.local", "/authority-port"),
              "route-level purge removes authority variants");
        const int purgedBefore = origin.hits();
        check(contains(
                  httpGet(edgePort, "front.local:80", "/authority-port"),
                  "X-Cache: MISS"),
              "purge removes the first port variant");
        check(contains(
                  httpGet(edgePort, "front.local:81", "/authority-port"),
                  "X-Cache: MISS"),
              "purge removes the second port variant");
        check(origin.hits() == purgedBefore + 2,
              "both purged authority variants are refetched");
    }

    // Range: partial content served from the cached full body (no origin hit).
    {
        const int before = origin.hits();
        const auto r1 = httpGetRange(edgePort, "front.local", "/page", "bytes=0-1");
        check(statusOf(r1) == 206, "range request returns 206");
        check(contains(r1, "Content-Range: bytes 0-1/5"),
              "Content-Range reflects the served slice");
        check(bodyOf(r1) == "he", "range body is the requested slice");

        const auto r2 = httpGetRange(edgePort, "front.local", "/page", "bytes=2-");
        check(statusOf(r2) == 206, "open-ended range returns 206");
        check(bodyOf(r2) == "llo", "open-ended range body is correct");

        const auto r3 = httpGetRange(edgePort, "front.local", "/page", "bytes=-2");
        check(statusOf(r3) == 206, "suffix range returns 206");
        check(bodyOf(r3) == "lo", "suffix range body is the last bytes");

        const auto r4 = httpGetRange(edgePort, "front.local", "/page", "bytes=10-20");
        check(statusOf(r4) == 416, "unsatisfiable range returns 416");
        check(contains(r4, "Content-Range: bytes */5"),
              "416 reports the full representation length");
        check(origin.hits() == before, "ranges are served from cache");
    }

    // A bodyless non-GET method is proxied to the origin and bypasses the cache.
    // (Uses a different target so it does not disturb the cached /page.)
    {
        const int before = origin.hits();
        const auto r = httpRaw(edgePort,
                               "DELETE /thing HTTP/1.1\r\nHost: front.local\r\n"
                               "Connection: close\r\n\r\n");
        check(statusOf(r) == 200, "DELETE is proxied to the origin");
        check(contains(r, "X-Cache: BYPASS"), "DELETE bypasses the cache");
        check(origin.hits() == before + 1, "DELETE contacted the origin");
    }

    // RFC 9110 Connection nominations are hop-by-hop on both proxy faces, not
    // merely the fixed legacy field names.
    {
        const auto response = httpRaw(
            edgePort,
            "GET /hop HTTP/1.1\r\nHost: front.local\r\n"
            "Connection: close, X-Client-Hop\r\n"
            "X-Client-Hop: client-secret\r\n"
            "X-End-To-End: retained\r\n\r\n");
        check(statusOf(response) == 200, "hop-by-hop filtering request served");
        const auto seen = origin.lastRequest();
        check(!contains(seen, "X-Client-Hop: client-secret"),
              "Connection-nominated request field is not forwarded");
        check(contains(seen, "X-End-To-End: retained"),
              "end-to-end request field is forwarded");
        check(!contains(response, "X-Origin-Hop: origin-secret"),
              "Connection-nominated origin field is not sent to the client");
        check(contains(response, "X-End-To-End: retained"),
              "end-to-end origin field is sent to the client");

        const auto cached = httpRaw(
            edgePort,
            "GET /hop HTTP/1.1\r\nHost: front.local\r\n"
            "Connection: close\r\n\r\n");
        check(contains(cached, "X-Cache: HIT"),
              "sanitized origin metadata remains cacheable");
        check(!contains(cached, "X-Origin-Hop: origin-secret"),
              "nominated origin field is absent from cached responses");
    }

    // Purging one URI invalidates every variant of exactly that URI, without
    // treating a longer target with the same text prefix as a match.
    {
        const auto neighbor = httpGet(edgePort, "front.local", "/page-extra");
        check(contains(neighbor, "X-Cache: MISS"),
              "neighboring target is cached before purge");
        check(edge.purge("front.local", "/page"), "purge reports a removed entry");
        const int before = origin.hits();
        const auto retained = httpGet(edgePort, "front.local", "/page-extra");
        check(contains(retained, "X-Cache: HIT"),
              "purge target boundary retains a longer neighboring URI");
        check(origin.hits() == before,
              "retained neighboring URI does not contact the origin");
        const auto r = httpGet(edgePort, "front.local", "/page");
        check(contains(r, "X-Cache: MISS"), "post-purge request is a MISS");
        check(origin.hits() == before + 1, "post-purge request re-contacted the origin");
    }

    // HEAD is answered from the cached GET: status and headers with the resource
    // length, but no message body, and without contacting the origin.
    {
        const int before = origin.hits();
        const auto r = httpHead(edgePort, "front.local", "/page");
        check(statusOf(r) == 200, "HEAD served 200");
        check(contains(r, "X-Cache: HIT"), "HEAD served from the cached GET");
        check(contains(r, "Content-Length: 5"), "HEAD reports the resource length");
        check(bodyOf(r).empty(), "HEAD response carries no body");
        check(origin.hits() == before, "HEAD hit did not contact the origin");
    }

    // A non-GET method bypasses the cache, forwards its body to the origin, and
    // invalidates the cached GET for the same target on success.
    {
        const auto post = httpPost(edgePort, "front.local", "/page", "payload=42");
        check(statusOf(post) == 200, "POST proxied with the origin status");
        check(contains(post, "X-Cache: BYPASS"), "POST bypasses the cache");
        check(contains(origin.lastRequest(), "payload=42"),
              "request body is forwarded to the origin");

        const auto get = httpGet(edgePort, "front.local", "/page");
        check(contains(get, "X-Cache: MISS"),
              "the unsafe method invalidated the cached GET");
    }

    // Client preconditions are end-to-end semantics. The conservative edge
    // policy forwards them rather than silently answering an unconditional
    // cached 200 or replacing them with its own revalidation validator.
    {
        const int before = origin.notModified();
        const auto conditional = httpRaw(
            edgePort,
            "GET /page HTTP/1.1\r\nHost: front.local\r\n"
            "If-None-Match: \"v1\"\r\nConnection: close\r\n\r\n");
        check(statusOf(conditional) == 304,
              "client If-None-Match reaches the origin and returns 304");
        check(contains(conditional, "X-Cache: BYPASS"),
              "conditional retrieval bypasses local cache evaluation");
        check(bodyOf(conditional).empty(), "304 carries no response body");
        check(origin.notModified() == before + 1,
              "origin evaluated the client's validator");
    }

    // Cache write-through and cache invalidation are different properties.
    // OPTIONS bypasses storage but is safe, so a successful OPTIONS response
    // must not invalidate the cached GET representation.
    {
        const auto fill = httpGet(edgePort, "front.local", "/safe-options");
        check(contains(fill, "X-Cache: MISS"),
              "safe-method invalidation fixture is cached");
        const int before = origin.hits();
        const auto options = httpRaw(
            edgePort,
            "OPTIONS /safe-options HTTP/1.1\r\nHost: front.local\r\n"
            "Connection: close\r\n\r\n");
        check(statusOf(options) == 200 && contains(options, "X-Cache: BYPASS"),
              "OPTIONS writes through to the origin");
        check(origin.hits() == before + 1,
              "OPTIONS contacted the origin exactly once");
        const auto retained = httpGet(
            edgePort, "front.local", "/safe-options");
        check(contains(retained, "X-Cache: HIT"),
              "safe OPTIONS does not invalidate cached GET");
        check(origin.hits() == before + 1,
              "retained GET cache entry avoids another origin request");
    }

    // Authenticated retrievals never enter or reuse the shared cache under the
    // conservative edge policy, preventing a max-age-only personalized response
    // from leaking into an anonymous request.
    {
        const int before = origin.hits();
        const auto authenticated = httpRaw(
            edgePort,
            "GET /auth-private HTTP/1.1\r\nHost: front.local\r\n"
            "Authorization: Bearer secret\r\nConnection: close\r\n\r\n");
        check(contains(authenticated, "X-Cache: BYPASS"),
              "authenticated retrieval bypasses the shared cache");
        const auto anonymous = httpRaw(
            edgePort,
            "GET /auth-private HTTP/1.1\r\nHost: front.local\r\n"
            "Connection: close\r\n\r\n");
        check(contains(anonymous, "X-Cache: MISS"),
              "authenticated response was not stored for anonymous reuse");
        check(origin.hits() == before + 2,
              "both authenticated and first anonymous requests reach origin");
        const auto anonymousHit = httpRaw(
            edgePort,
            "GET /auth-private HTTP/1.1\r\nHost: front.local\r\n"
            "Connection: close\r\n\r\n");
        check(contains(anonymousHit, "X-Cache: HIT"),
              "ordinary anonymous retrieval remains cacheable");
    }

    // Request cache directives constrain reuse and storage. no-cache (including
    // legacy Pragma fallback) reaches the origin, no-store cannot seed a new
    // entry, and only-if-cached never contacts the origin on a miss.
    {
        const auto fill = httpRaw(
            edgePort,
            "GET /directives HTTP/1.1\r\nHost: front.local\r\n"
            "Connection: close\r\n\r\n");
        check(contains(fill, "X-Cache: MISS"),
              "request-directive fixture is initially cached");

        int before = origin.hits();
        const auto revalidate = httpRaw(
            edgePort,
            "GET /directives HTTP/1.1\r\nHost: front.local\r\n"
            "Cache-Control: no-cache\r\nConnection: close\r\n\r\n");
        check(contains(revalidate, "X-Cache: BYPASS"),
              "request no-cache does not reuse a stored response directly");
        check(origin.hits() == before + 1,
              "request no-cache reaches the origin");

        before = origin.hits();
        const auto pragma = httpRaw(
            edgePort,
            "GET /directives HTTP/1.1\r\nHost: front.local\r\n"
            "Pragma: no-cache\r\nConnection: close\r\n\r\n");
        check(contains(pragma, "X-Cache: BYPASS"),
              "legacy Pragma no-cache is honored without Cache-Control");
        check(origin.hits() == before + 1,
              "legacy Pragma no-cache reaches the origin");

        before = origin.hits();
        const auto cachedOnlyHit = httpRaw(
            edgePort,
            "GET /directives HTTP/1.1\r\nHost: front.local\r\n"
            "Cache-Control: only-if-cached\r\nConnection: close\r\n\r\n");
        check(contains(cachedOnlyHit, "X-Cache: HIT"),
              "only-if-cached can use an existing fresh response");
        check(origin.hits() == before,
              "only-if-cached hit does not contact the origin");

        const auto cachedOnlyMiss = httpRaw(
            edgePort,
            "GET /only-if-cached-miss HTTP/1.1\r\nHost: front.local\r\n"
            "Cache-Control: only-if-cached\r\nConnection: close\r\n\r\n");
        check(statusOf(cachedOnlyMiss) == 504,
              "only-if-cached miss returns 504");
        check(origin.hits() == before,
              "only-if-cached miss does not contact the origin");

        before = origin.hits();
        const auto noStore = httpRaw(
            edgePort,
            "GET /request-no-store HTTP/1.1\r\nHost: front.local\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
        check(contains(noStore, "X-Cache: BYPASS"),
              "request no-store bypasses storage");
        const auto afterNoStore = httpRaw(
            edgePort,
            "GET /request-no-store HTTP/1.1\r\nHost: front.local\r\n"
            "Connection: close\r\n\r\n");
        check(contains(afterNoStore, "X-Cache: MISS"),
              "response to request no-store was not retained");
        check(origin.hits() == before + 2,
              "no-store exchange and later fill both reach the origin");
    }

    // Control plane: the origin table mutates at runtime through the member
    // functions (the library exposes no built-in HTTP admin surface -- an
    // embedding app builds one on top of these if it wants one).
    {
        check(edge.addOrigin("admin.local",
                             OriginSettings{"127.0.0.1", origin.port(), false}),
              "addOrigin maps a second host at runtime");
        const auto proxied = httpGet(edgePort, "admin.local", "/admin-page");
        check(statusOf(proxied) == 200, "the newly added origin proxies requests");

        check(edge.removeOrigin("admin.local"), "removeOrigin drops the mapping");
        const auto gone = httpGet(edgePort, "admin.local", "/admin-page");
        check(statusOf(gone) == 502, "removed origin is no longer routable");
    }

    // Keep-alive: two requests are served on one persistent connection.
    {
        const auto [first, second] = httpKeepAliveTwo(edgePort, "front.local", "/page");
        check(statusOf(first) == 200, "keep-alive request 1 served");
        check(contains(first, "Connection: keep-alive"),
              "response 1 signals the connection stays open");
        check(bodyOf(first) == "hello", "keep-alive body 1 is correct");
        check(statusOf(second) == 200,
              "keep-alive request 2 served on the same connection");
        check(bodyOf(second) == "hello", "keep-alive body 2 is correct");
        check(contains(second, "Connection: close"),
              "response 2 closes the connection as requested");
    }

    // Connection persistence is the shared ruvia-http token plan, not a
    // substring search. An unrelated option containing "close" must not close
    // an HTTP/1.1 connection, and the response version follows the request.
    {
        const auto [first, second] = httpKeepAliveTwo(
            edgePort, "front.local", "/page", "HTTP/1.1", "disclose");
        check(statusOf(first) == 200 && statusOf(second) == 200,
              "an unrelated Connection token does not disable HTTP/1.1 reuse");

        const auto [http10First, http10Second] = httpKeepAliveTwo(
            edgePort, "front.local", "/page", "HTTP/1.0", "keep-alive");
        check(http10First.starts_with("HTTP/1.0 200"),
              "HTTP/1.0 request receives an HTTP/1.0 response");
        check(contains(http10First, "Connection: keep-alive"),
              "HTTP/1.0 keep-alive token enables reuse");
        check(http10Second.starts_with("HTTP/1.0 200"),
              "a second HTTP/1.0 request is served on the reused connection");
    }

    // HTTP/1.0 cannot use chunked transfer coding. An unknown-length origin
    // stream is therefore close-delimited even if the request asked to persist.
    {
        const auto response = httpRaw(
            edgePort,
            "GET /chunked10 HTTP/1.0\r\nHost: front.local\r\n"
            "Connection: keep-alive\r\n\r\n");
        check(response.starts_with("HTTP/1.0 200"),
              "close-delimited streaming preserves HTTP/1.0");
        check(!contains(response, "Transfer-Encoding: chunked"),
              "HTTP/1.0 streaming never emits chunked framing");
        check(contains(response, "Connection: close"),
              "unknown-length HTTP/1.0 stream closes for delimiting");
        check(bodyOf(response) == "hello world",
              "close-delimited HTTP/1.0 body is forwarded without chunk frames");
    }

    // Parse failures retain the status chosen by the shared protocol core.
    {
        const auto tooLarge = httpRaw(
            edgePort,
            "POST /oversize HTTP/1.1\r\nHost: front.local\r\n"
            "Content-Length: 1048577\r\nConnection: close\r\n\r\n");
        check(statusOf(tooLarge) == 413,
              "edge rejects a declared body above its 1 MB buffered limit");

        const auto unsupported = httpRaw(
            edgePort,
            "GET / HTTP/9.9\r\nHost: front.local\r\nConnection: close\r\n\r\n");
        check(statusOf(unsupported) == 505,
              "unsupported request version uses the protocol core's 505 status");
    }

    // Streaming: an unknown-length (chunked) origin response is streamed to the
    // client re-chunked (not buffered), and is still cached; the cached hit is
    // then served buffered.
    {
        const auto r = httpGet(edgePort, "front.local", "/chunked");
        check(statusOf(r) == 200, "chunked response streamed with 200");
        check(contains(r, "Transfer-Encoding: chunked"),
              "unknown-length body is re-chunked to the client");
        check(contains(r, "X-Cache: MISS"), "first chunked request is a MISS");
        check(dechunk(bodyOf(r)) == "hello world",
              "streamed chunked body decodes to the origin content");

        const auto r2 = httpGet(edgePort, "front.local", "/chunked");
        check(contains(r2, "X-Cache: HIT"), "chunked response was cached");
        check(bodyOf(r2) == "hello world", "cached body served buffered on the hit");
    }

    // Vary + compressed variants: distinct Accept-Encoding values cache
    // separately, and a response that Varies on an uncovered field is not cached.
    {
        const int base = origin.hits();
        (void)httpGetEnc(edgePort, "front.local", "/vary", "gzip");  // gzip variant MISS
        (void)httpGetEnc(edgePort, "front.local", "/vary", "br");    // br variant MISS
        check(origin.hits() == base + 2,
              "distinct Accept-Encoding values fetch distinct variants");
        const auto again = httpGetEnc(edgePort, "front.local", "/vary", "gzip");
        check(contains(again, "X-Cache: HIT"), "the gzip variant was cached");
        check(origin.hits() == base + 2, "a cached variant is not refetched");

        const int weightedBase = origin.hits();
        (void)httpGetEnc(
            edgePort, "front.local", "/vary-q", "gzip;q=1, br;q=0.1");
        (void)httpGetEnc(
            edgePort, "front.local", "/vary-q", "gzip;q=0.1, br;q=1");
        check(origin.hits() == weightedBase + 2,
              "different Accept-Encoding weights select distinct variants");

        const int repeatedBase = origin.hits();
        (void)httpRaw(
            edgePort,
            "GET /vary-repeat HTTP/1.1\r\nHost: front.local\r\n"
            "Accept-Encoding: gzip;q=1\r\nAccept-Encoding: br;q=0\r\n"
            "Connection: close\r\n\r\n");
        (void)httpRaw(
            edgePort,
            "GET /vary-repeat HTTP/1.1\r\nHost: front.local\r\n"
            "Accept-Encoding: gzip;q=1\r\nAccept-Encoding: br;q=1\r\n"
            "Connection: close\r\n\r\n");
        check(origin.hits() == repeatedBase + 2,
              "every repeated Accept-Encoding field line participates in the key");

        const int emptyBase = origin.hits();
        (void)httpRaw(
            edgePort,
            "GET /vary-empty HTTP/1.1\r\nHost: front.local\r\n"
            "Connection: close\r\n\r\n");
        (void)httpRaw(
            edgePort,
            "GET /vary-empty HTTP/1.1\r\nHost: front.local\r\n"
            "Accept-Encoding:\r\nConnection: close\r\n\r\n");
        check(origin.hits() == emptyBase + 2,
              "absent and present-empty Accept-Encoding remain distinct");

        const int base2 = origin.hits();
        (void)httpGetEnc(edgePort, "front.local", "/varycookie", "gzip");
        const auto cookie2 = httpGetEnc(edgePort, "front.local", "/varycookie", "gzip");
        check(contains(cookie2, "X-Cache: MISS"),
              "a Vary: Cookie response is not cached");
        check(origin.hits() == base2 + 2, "Vary: Cookie is refetched every time");
    }

    // stale-while-revalidate: a stale-but-in-window entry is served immediately
    // while the origin is revalidated in the background, and the refreshed entry
    // is then a fresh hit.
    {
        constexpr std::string_view kWeightedEncoding =
            "gzip;q=0.2, br;q=0.8";
        const auto r1 = httpGetEnc(
            edgePort, "front.local", "/swr", kWeightedEncoding);
        check(contains(r1, "X-Cache: MISS"), "first /swr request is a MISS");

        std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // now stale, in window

        const int fullBefore = origin.hits();
        const int notModifiedBefore = origin.notModified();
        const auto r2 = httpGetEnc(
            edgePort, "front.local", "/swr", kWeightedEncoding);
        check(contains(r2, "X-Cache: STALE"),
              "stale-while-revalidate serves the stale copy immediately");
        check(bodyOf(r2) == "hello", "SWR serves the stale body");
        check(origin.hits() == fullBefore, "SWR did not block on a foreground fetch");

        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let it refresh
        check(origin.notModified() == notModifiedBefore + 1,
              "the background job revalidated with the origin");
        check(contains(
                  origin.lastRequest(),
                  "Accept-Encoding: gzip;q=0.2, br;q=0.8"),
              "background revalidation preserves the exact variant selector");

        const auto r3 = httpGetEnc(
            edgePort, "front.local", "/swr", kWeightedEncoding);
        check(contains(r3, "X-Cache: HIT"), "the background-refreshed entry is fresh");
    }

    // Request coalescing: many concurrent misses for the same (slow) origin key
    // collapse into a single origin fetch, and every client still gets served.
    {
        const int before = origin.hits();
        constexpr int kClients = 5;
        std::vector<std::thread> clients;
        std::vector<std::string> responses(kClients);
        for (int i = 0; i < kClients; ++i) {
            clients.emplace_back(
                [&, i] { responses[i] = httpGet(edgePort, "front.local", "/slow"); });
        }
        for (auto& t : clients) {
            t.join();
        }
        for (const auto& r : responses) {
            check(statusOf(r) == 200, "coalesced client received 200");
            check(bodyOf(r) == "hello", "coalesced client received the body");
        }
        check(origin.hits() == before + 1,
              "concurrent misses coalesced into a single origin fetch");
    }

    // A routing mutation is serialized onto the worker but cannot invalidate
    // the lease held by a request already suspended in origin I/O.
    {
        EdgeServer leaseEdge(ruvia::edge::EdgeEndpoint{"127.0.0.1", 0});
        check(leaseEdge.addOrigin(
                  "lease.local",
                  OriginSettings{"127.0.0.1", origin.port(), false}),
              "lease fixture origin is registered");
        leaseEdge.start();
        const auto port = leaseEdge.localEndpoint().port;
        const int before = origin.hits();
        std::string inFlightResponse;
        std::thread inFlight(
            [&] { inFlightResponse = httpGet(port, "lease.local", "/slow"); });
        for (int attempt = 0; attempt < 100 && origin.hits() == before; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(leaseEdge.removeOrigin("lease.local"),
              "control-plane removal is applied while a request is suspended");
        inFlight.join();
        check(statusOf(inFlightResponse) == 200,
              "in-flight request retains a stable origin lease after removal");
        check(statusOf(httpGet(port, "lease.local", "/slow")) == 502,
              "a later request observes the removed mapping");
        leaseEdge.stop();
    }

    // Observability: the access-log callback sees each request's cache result,
    // giving an embedding app the raw material to build metrics of its own.
    {
        std::mutex logMutex;
        std::vector<std::string> logResults;
        EdgeServer* observed = nullptr;
        bool callbackAddedOrigin = false;
        ruvia::edge::EdgeServerOptions obsOptions;
        obsOptions.accessLog = [&](const ruvia::edge::AccessLogEntry& e) {
            std::lock_guard<std::mutex> guard(logMutex);
            logResults.emplace_back(e.cacheResult);
            if (!callbackAddedOrigin) {
                callbackAddedOrigin = observed->addOrigin(
                    "callback.local",
                    OriginSettings{"127.0.0.1", origin.port(), false});
            }
        };
        EdgeServer obsEdge(ruvia::edge::EdgeEndpoint{"0.0.0.0", 0}, std::move(obsOptions));
        observed = &obsEdge;
        obsEdge.start();
        check(obsEdge.addOrigin(
                  "front.local",
                  OriginSettings{"127.0.0.1", origin.port(), false}),
              "observability fixture origin is registered");
        const std::uint16_t obsPort = obsEdge.localEndpoint().port;

        (void)httpGet(obsPort, "front.local", "/page");  // MISS
        (void)httpGet(obsPort, "front.local", "/page");  // HIT
        check(statusOf(httpGet(obsPort, "callback.local", "/callback")) == 200,
              "worker callback may invoke the serialized control plane directly");

        {
            std::lock_guard<std::mutex> guard(logMutex);
            check(logResults.size() == 3, "each request produced an access-log entry");
            check(logResults.size() == 3 && logResults[0] == "MISS",
                  "first request logged as a MISS");
            check(logResults.size() == 3 && logResults[1] == "HIT",
                  "second request logged as a HIT");
            check(callbackAddedOrigin, "access-log callback added an origin on the worker");
        }
        obsEdge.stop();
    }

    // Logging is an observational side effect. A callback failure must not
    // escape the request-record destructor (which would otherwise terminate the
    // process), truncate the response, or poison later requests on the worker.
    {
        std::atomic<int> logAttempts{0};
        ruvia::edge::EdgeServerOptions logFailureOptions;
        logFailureOptions.accessLog = [&](const ruvia::edge::AccessLogEntry&) {
            logAttempts.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("access log failed");
        };
        EdgeServer logFailureEdge(
            ruvia::edge::EdgeEndpoint{"127.0.0.1", 0},
            std::move(logFailureOptions));
        logFailureEdge.start();
        check(logFailureEdge.addOrigin(
                  "front.local",
                  OriginSettings{"127.0.0.1", origin.port(), false}),
              "throwing-log fixture origin is registered");
        const auto port = logFailureEdge.localEndpoint().port;
        check(statusOf(httpGet(port, "front.local", "/log-failure-one")) == 200,
              "throwing access log does not truncate its request");
        check(statusOf(httpGet(port, "front.local", "/log-failure-two")) == 200,
              "worker remains usable after an access-log exception");
        check(logAttempts.load(std::memory_order_relaxed) == 2,
              "each completed request still attempts access logging");
        logFailureEdge.stop();
    }

    // Client-side TLS termination: a separate TLS edge in front of the same
    // origin serves an HTTPS request end to end.
    {
        ruvia::edge::EdgeServerOptions tlsOptions;
        tlsOptions.tls = ruvia::edge::EdgeTlsConfig{
            std::string(edge_test_tls::kCertPem), std::string(edge_test_tls::kKeyPem)};
        EdgeServer tlsEdge(ruvia::edge::EdgeEndpoint{"0.0.0.0", 0}, std::move(tlsOptions));
        tlsEdge.start();
        check(tlsEdge.addOrigin(
                  "front.local",
                  OriginSettings{"127.0.0.1", origin.port(), false}),
              "TLS fixture origin is registered");
        const std::uint16_t tlsPort = tlsEdge.localEndpoint().port;

        const auto r = httpsGet(tlsPort, "front.local", "/page");
        check(statusOf(r) == 200, "TLS-terminated request served with 200");
        check(bodyOf(r) == "hello", "TLS request proxied to the origin");
        check(contains(origin.lastRequest(), "X-Forwarded-Proto: https"),
              "TLS termination forwards the https scheme to the origin");

        // Rotate the certificate through the member API; HTTPS keeps working.
        const ruvia::edge::EdgeTlsConfig fresh{
            std::string(edge_test_tls::kCertPem), std::string(edge_test_tls::kKeyPem)};
        check(tlsEdge.setTlsCertificate(fresh), "setTlsCertificate rotates the certificate");
        const auto afterRotate = httpsGet(tlsPort, "front.local", "/page");
        check(statusOf(afterRotate) == 200, "HTTPS still works after rotation");

        // Invalid PEM is rejected.
        check(!tlsEdge.setTlsCertificate(ruvia::edge::EdgeTlsConfig{"not a cert", "not a key"}),
              "setTlsCertificate rejects invalid PEM");
        tlsEdge.stop();

        // A plaintext edge has no certificate to rotate.
        check(!edge.setTlsCertificate(fresh),
              "setTlsCertificate fails when TLS is not enabled");
    }

    // Direct shutdown: stop() does not wait for an in-flight origin request.
    {
        EdgeServer stoppingEdge(ruvia::edge::EdgeEndpoint{"0.0.0.0", 0}, {});
        stoppingEdge.start();
        check(stoppingEdge.addOrigin(
                  "front.local",
                  OriginSettings{"127.0.0.1", origin.port(), false}),
              "shutdown fixture origin is registered");
        const std::uint16_t port = stoppingEdge.localEndpoint().port;

        std::string result;
        std::thread inflight([&] { result = httpGet(port, "front.local", "/slow"); });
        // Let the request reach the origin (which delays 300ms), then stop now.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto started = std::chrono::steady_clock::now();
        stoppingEdge.stop();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        inflight.join();

        check(elapsed < std::chrono::milliseconds(200),
              "stop does not drain an in-flight request");
        check(statusOf(result) != 200, "direct stop interrupts the in-flight response");
    }

    // Structured shutdown also owns background SWR refreshes. Cancelling the
    // server must join that coroutine promptly instead of abandoning its frame
    // inside io_context until after the cache/config members are destroyed.
    {
        EdgeServer refreshEdge(ruvia::edge::EdgeEndpoint{"127.0.0.1", 0});
        check(refreshEdge.addOrigin(
                  "front.local",
                  OriginSettings{"127.0.0.1", origin.port(), false}),
              "refresh fixture origin is registered");
        refreshEdge.start();
        const auto port = refreshEdge.localEndpoint().port;

        const auto initial = httpGet(port, "front.local", "/swrslow");
        check(contains(initial, "X-Cache: MISS"),
              "slow-refresh fixture is initially cached");
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));

        const int refreshesBefore = origin.slowRevalidations();
        const auto stale = httpGet(port, "front.local", "/swrslow");
        check(contains(stale, "X-Cache: STALE"),
              "slow background revalidation serves stale immediately");
        for (int attempt = 0;
             attempt < 100 && origin.slowRevalidations() == refreshesBefore;
             ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(origin.slowRevalidations() == refreshesBefore + 1,
              "slow background revalidation entered origin I/O");

        const auto started = std::chrono::steady_clock::now();
        refreshEdge.stop();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        check(elapsed < std::chrono::milliseconds(200),
              "stop cancels and joins a tracked background refresh promptly");
    }

    // Persistent disk tier: an entry cached by one edge is served from disk by a
    // second edge over the same directory -- a fresh, empty memory cache, and the
    // origin is not re-contacted.
    {
        const std::filesystem::path diskDir =
            std::filesystem::temp_directory_path() / "ruvia_edge_disk_test";
        std::error_code ec;
        std::filesystem::remove_all(diskDir, ec);

        {
            ruvia::edge::EdgeServerOptions diskOptions;
            diskOptions.cacheDirectory = diskDir;
            EdgeServer diskEdge(ruvia::edge::EdgeEndpoint{"0.0.0.0", 0}, std::move(diskOptions));
            diskEdge.join();  // pre-start join must not poison the disk executor
            diskEdge.start();
            check(diskEdge.addOrigin(
                      "front.local",
                      OriginSettings{"127.0.0.1", origin.port(), false}),
                  "disk fixture origin is registered");
            const std::uint16_t diskPort = diskEdge.localEndpoint().port;
            const auto miss = httpGet(diskPort, "front.local", "/diskpage");
            check(contains(miss, "X-Cache: MISS"), "first disk-tier request is a MISS");
            check(bodyOf(miss) == "hello", "disk-tier MISS proxied the origin body");
            // stop() must drain a just-enqueued persistent write; callers must
            // not need an arbitrary delay to make an acknowledged fill durable.
            diskEdge.stop();
        }

        const int before = origin.hits();
        {
            ruvia::edge::EdgeServerOptions reopenOptions;
            reopenOptions.cacheDirectory = diskDir;
            EdgeServer reopened(
                ruvia::edge::EdgeEndpoint{"0.0.0.0", 0},
                std::move(reopenOptions));
            reopened.start();
            check(reopened.addOrigin(
                      "front.local",
                      OriginSettings{"127.0.0.1", origin.port(), false}),
                  "reopened disk fixture origin is registered");
            const std::uint16_t reopenPort = reopened.localEndpoint().port;
            const auto hit = httpGet(reopenPort, "front.local", "/diskpage");
            check(contains(hit, "X-Cache: HIT"),
                  "a fresh edge serves the entry from the disk tier");
            check(bodyOf(hit) == "hello", "disk-tier HIT body matches the origin");
            check(origin.hits() == before,
                  "disk-tier HIT did not re-contact the origin");
            check(reopened.purge("front.local", "/diskpage"),
                  "disk-backed purge waits for durable removal");
            reopened.stop();
        }

        const int afterPurge = origin.hits();
        {
            ruvia::edge::EdgeServerOptions purgedOptions;
            purgedOptions.cacheDirectory = diskDir;
            EdgeServer purged(
                ruvia::edge::EdgeEndpoint{"0.0.0.0", 0},
                std::move(purgedOptions));
            purged.start();
            check(purged.addOrigin(
                      "front.local",
                      OriginSettings{"127.0.0.1", origin.port(), false}),
                  "purged disk fixture origin is registered");
            const auto refetched = httpGet(
                purged.localEndpoint().port, "front.local", "/diskpage");
            check(contains(refetched, "X-Cache: MISS"),
                  "a reopened edge observes the completed disk purge");
            check(origin.hits() == afterPurge + 1,
                  "durably purged entry is refetched from the origin");
            check(purged.clearCache(),
                  "disk-backed clear reports complete durable removal");
            const int afterClear = origin.hits();
            const auto afterClearResponse = httpGet(
                purged.localEndpoint().port, "front.local", "/diskpage");
            check(contains(afterClearResponse, "X-Cache: MISS"),
                  "clear removes both memory and disk tiers");
            check(origin.hits() == afterClear + 1,
                  "request after clear is refetched from the origin");
            purged.stop();
        }
        std::filesystem::remove_all(diskDir, ec);
    }

    // HTTP/2: ALPN negotiation, streamed responses, flow control across a body
    // larger than the window, and true multiplexing of concurrent streams.
    //
    // This block drives a real h2 client, so it is compiled only where the
    // build found nghttp and h2load. CMake reports the skip at configure time.
#if !defined(_WIN32) && defined(RUVIA_NGHTTP_EXECUTABLE) && defined(RUVIA_H2LOAD_EXECUTABLE)
    {
        ruvia::edge::EdgeServerOptions h2Options;
        h2Options.tls = ruvia::edge::EdgeTlsConfig{
            std::string(edge_test_tls::kCertPem), std::string(edge_test_tls::kKeyPem)};
        EdgeServer h2Edge(ruvia::edge::EdgeEndpoint{"0.0.0.0", 0}, std::move(h2Options));
        h2Edge.start();
        check(h2Edge.addOrigin(
                  "front.local",
                  OriginSettings{"127.0.0.1", origin.port(), false}),
              "HTTP/2 fixture authority is registered");
        // h2load addresses the edge by IP, so map that authority to the origin too.
        check(h2Edge.addOrigin(
                  "127.0.0.1",
                  OriginSettings{"127.0.0.1", origin.port(), false}),
              "HTTP/2 IP authority is registered");
        const std::uint16_t h2Port = h2Edge.localEndpoint().port;
        const std::string base = "https://127.0.0.1:" + std::to_string(h2Port);

        // 1. Basic request served over an ALPN-negotiated h2 connection.
        const std::string page = runShell(
            "\"" RUVIA_NGHTTP_EXECUTABLE "\" -y " + base + "/page");
        check(page == "hello",
              "h2 request served over ALPN and proxied the origin body");

        // 2. A chunked (unknown-length) origin response is streamed over h2.
        const std::string chunked = runShell(
            "\"" RUVIA_NGHTTP_EXECUTABLE "\" -y " + base + "/chunked");
        check(chunked == "hello world",
              "h2 streamed body reassembles to the origin content");

        // 3. A body far larger than the 65535-byte flow-control window streams in
        //    full, exercising window exhaustion and WINDOW_UPDATE-driven resume.
        const std::string big = runShell(
            "\"" RUVIA_NGHTTP_EXECUTABLE "\" -y " + base + "/bigchunk");
        check(big.size() == 200000,
              "h2 delivered the full flow-controlled body");

        // 4. True multiplexing: many concurrent streams on one connection all
        //    complete without head-of-line blocking or deadlock.
        const std::string mux = runShell(
            "\"" RUVIA_H2LOAD_EXECUTABLE "\" -n16 -c1 -m8 " +
            base + "/bigchunk");
        check(contains(mux, "16 succeeded"),
              "16 concurrent h2 streams all succeeded");

        // 5. Buffered request DATA returns receive-window credit only after its
        //    borrowed event bytes are copied. Three sequential 600 KB uploads
        //    exceed one connection window in aggregate and therefore cannot all
        //    finish unless the edge emits WINDOW_UPDATE between requests.
        const auto uploadPath =
            std::filesystem::temp_directory_path() /
            "ruvia_edge_h2_upload.bin";
        const auto writeUpload = [&](std::size_t bytes) {
            std::ofstream output(
                uploadPath,
                std::ios::binary | std::ios::trunc);
            const std::string block(64 * 1024, 'U');
            while (output && bytes != 0) {
                const auto count = (std::min)(bytes, block.size());
                output.write(
                    block.data(),
                    static_cast<std::streamsize>(count));
                bytes -= count;
            }
            return output.good();
        };
        check(writeUpload(600000), "h2 upload fixture was written");
        const std::string uploads = runShell(
            "\"" RUVIA_H2LOAD_EXECUTABLE
            "\" -n3 -c1 -m1 -d \"" +
            uploadPath.string() + "\" " + base + "/upload");
        check(contains(uploads, "3 succeeded"),
              "sequential h2 request bodies return connection window credit");

        // The HTTP/1 and HTTP/2 faces share the same bounded-buffer policy.
        // A declared body above 1 MB is rejected at the stream boundary instead
        // of being accumulated without limit.
        check(writeUpload(1024 * 1024 + 1),
              "oversized h2 upload fixture was written");
        const std::string oversized = runShell(
            "\"" RUVIA_H2LOAD_EXECUTABLE
            "\" -n1 -c1 -m1 -d \"" +
            uploadPath.string() + "\" " + base + "/oversized-upload");
        check(contains(oversized, "1 failed"),
              "oversized h2 request body is reset before proxying");
        std::error_code uploadRemoveError;
        std::filesystem::remove(uploadPath, uploadRemoveError);
        h2Edge.stop();
    }
#endif

    // Conditional revalidation: a short-lived entry goes stale, is revalidated
    // with the origin, comes back 304, and is served from cache without a full
    // transfer -- then a follow-up is a plain hit again.
    {
        const auto a = httpGet(edgePort, "front.local", "/rev");
        check(contains(a, "X-Cache: MISS"), "first /rev request is a MISS");
        check(bodyOf(a) == "hello", "/rev body proxied");

        std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // let it go stale

        const int fullBefore = origin.hits();
        const int notModifiedBefore = origin.notModified();
        const auto b = httpGet(edgePort, "front.local", "/rev");
        check(contains(b, "X-Cache: REVALIDATED"), "stale entry is revalidated");
        check(statusOf(b) == 200, "revalidated response is 200 from cache");
        check(bodyOf(b) == "hello", "revalidated body served from cache");
        check(contains(b, "Content-Type: text/plain"),
              "revalidated response keeps the stored representation headers");
        check(origin.notModified() == notModifiedBefore + 1,
              "origin answered the conditional with 304");
        check(origin.hits() == fullBefore, "revalidation sent no full response");

        const auto c = httpGet(edgePort, "front.local", "/rev");
        check(contains(c, "X-Cache: HIT"), "refreshed entry is a hit again");
        check(contains(c, "Content-Type: text/plain"),
              "the refreshed cache entry retained its headers");
    }

    // A full no-store replacement withdraws the old stale representation. It
    // cannot later be resurrected merely because the withdrawn entry used to
    // carry stale-if-error.
    {
        const auto initial = httpGet(
            edgePort, "front.local", "/nostore-transition");
        check(contains(initial, "X-Cache: MISS") && bodyOf(initial) == "hello",
              "no-store transition fixture starts as a cacheable response");
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));

        const auto withdrawn = httpGet(
            edgePort, "front.local", "/nostore-transition");
        check(statusOf(withdrawn) == 200 && bodyOf(withdrawn) == "newer",
              "origin replaces the stale representation with no-store content");

        const auto failure = httpGet(
            edgePort, "front.local", "/nostore-transition");
        check(statusOf(failure) == 500,
              "withdrawn stale entry cannot reappear through stale-if-error");
        check(!contains(failure, "X-Cache: STALE"),
              "no-store replacement removed the previous cached response");
    }

    // stale-if-error: once the origin is unreachable, a stale entry within its
    // stale-if-error window is served instead of a gateway error.
    {
        const auto a = httpGet(edgePort, "front.local", "/sie");
        check(contains(a, "X-Cache: MISS"), "first /sie request is a MISS");
        check(bodyOf(a) == "hello", "/sie body proxied");

        std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // go stale
        origin.stop();  // the origin is now unreachable

        const auto b = httpGet(edgePort, "front.local", "/sie");
        check(contains(b, "X-Cache: STALE"),
              "unreachable origin falls back to the stale copy");
        check(statusOf(b) == 200, "stale fallback keeps the stored status");
        check(bodyOf(b) == "hello", "stale body served on origin error");
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
