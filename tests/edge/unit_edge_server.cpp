// End-to-end proof of the edge node: a real EdgeServer in front of a loopback
// origin that counts how often it is hit. It checks that a first request is a
// proxied MISS, a repeat is served from cache as a HIT without touching the
// origin, a runtime purge forces the next request back to the origin, a
// non-GET method is rejected, and a runtime removeOrigin makes the mapping
// disappear -- exercising the dynamic add/remove-config and cache control the
// whole feature is about.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
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

#include "edge_tls_fixture.h"
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
        // Close the listener so later connects are refused promptly (the
        // stale-if-error test relies on a fast connection failure).
        asio::error_code ignore;
        acceptor_.close(ignore);
    }

    [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
    [[nodiscard]] int hits() const { return hits_.load(); }              // full 200s served
    [[nodiscard]] int notModified() const { return notModified_.load(); }  // 304s served
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
        if (request.find("If-None-Match: \"v1\"") != std::string::npos) {
            // Revalidation refreshes the entry with a longer, stable freshness.
            notModified_.fetch_add(1);
            response =
                "HTTP/1.1 304 Not Modified\r\n"
                "ETag: \"v1\"\r\n"
                "Cache-Control: max-age=60\r\n"
                "\r\n";
        } else if (request.find("GET /chunked ") != std::string::npos) {
            // An unknown-length (chunked) origin response.
            hits_.fetch_add(1);
            response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Cache-Control: max-age=60\r\n"
                "\r\n"
                "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        } else if (request.find("GET /vary ") != std::string::npos) {
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
        } else if (request.find("GET /swr ") != std::string::npos) {
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
    std::mutex mutex_;
    std::string lastRequest_;
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
    std::string_view target) {
    asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

    const auto send = [&](std::string_view connection) {
        std::string request = "GET ";
        request.append(target);
        request.append(" HTTP/1.1\r\nHost: ");
        request.append(host);
        request.append("\r\nConnection: ");
        request.append(connection);
        request.append("\r\n\r\n");
        asio::write(socket, asio::buffer(request));
    };

    send("keep-alive");
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
    options.adminEndpoint = tcp::endpoint(tcp::v4(), 0);
    EdgeServer edge(tcp::endpoint(tcp::v4(), 0), options);
    edge.start();
    check(edge.addOrigin("front.local",
                         OriginSettings{"127.0.0.1", origin.port(), false}),
          "addOrigin maps the front host at runtime");
    const std::uint16_t edgePort = edge.localEndpoint().port();
    const std::uint16_t adminPort = edge.localAdminEndpoint().value().port();

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

    // Purging the entry forces the next request back to the origin.
    {
        check(edge.purge("front.local", "/page"), "purge reports a removed entry");
        const int before = origin.hits();
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

    // Management API: add an origin over HTTP, use it on the data port, read
    // stats, then remove it -- the operator face of the dynamic config.
    {
        const std::string put =
            "PUT /origins/admin.local?upstream=127.0.0.1&port=" +
            std::to_string(origin.port()) +
            " HTTP/1.1\r\nHost: admin\r\nConnection: close\r\n\r\n";
        const auto putResp = httpRaw(adminPort, put);
        check(statusOf(putResp) == 200, "admin PUT /origins adds a mapping");
        check(contains(putResp, "created"), "admin reports the mapping was created");

        const auto proxied = httpGet(edgePort, "admin.local", "/admin-page");
        check(statusOf(proxied) == 200, "the admin-added origin proxies requests");

        const auto stats = httpRaw(
            adminPort, "GET /stats HTTP/1.1\r\nHost: admin\r\nConnection: close\r\n\r\n");
        check(statusOf(stats) == 200, "admin GET /stats works");
        check(contains(stats, "entries="), "stats reports cache entries");

        const auto del = httpRaw(
            adminPort,
            "DELETE /origins/admin.local HTTP/1.1\r\nHost: admin\r\nConnection: close\r\n\r\n");
        check(statusOf(del) == 200, "admin DELETE /origins removes the mapping");

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
        const auto r1 = httpGet(edgePort, "front.local", "/swr");
        check(contains(r1, "X-Cache: MISS"), "first /swr request is a MISS");

        std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // now stale, in window

        const int fullBefore = origin.hits();
        const int notModifiedBefore = origin.notModified();
        const auto r2 = httpGet(edgePort, "front.local", "/swr");
        check(contains(r2, "X-Cache: STALE"),
              "stale-while-revalidate serves the stale copy immediately");
        check(bodyOf(r2) == "hello", "SWR serves the stale body");
        check(origin.hits() == fullBefore, "SWR did not block on a foreground fetch");

        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let it refresh
        check(origin.notModified() == notModifiedBefore + 1,
              "the background job revalidated with the origin");

        const auto r3 = httpGet(edgePort, "front.local", "/swr");
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

    // Observability: an access-log callback sees each request's cache result, and
    // /stats reports the aggregate counters.
    {
        std::mutex logMutex;
        std::vector<std::string> logResults;
        ruvia::edge::EdgeServerOptions obsOptions;
        obsOptions.adminEndpoint = tcp::endpoint(tcp::v4(), 0);
        obsOptions.accessLog = [&](const ruvia::edge::AccessLogEntry& e) {
            std::lock_guard<std::mutex> guard(logMutex);
            logResults.emplace_back(e.cacheResult);
        };
        EdgeServer obsEdge(tcp::endpoint(tcp::v4(), 0), std::move(obsOptions));
        obsEdge.start();
        obsEdge.addOrigin("front.local",
                          OriginSettings{"127.0.0.1", origin.port(), false});
        const std::uint16_t obsPort = obsEdge.localEndpoint().port();
        const std::uint16_t obsAdmin = obsEdge.localAdminEndpoint().value().port();

        (void)httpGet(obsPort, "front.local", "/page");  // MISS
        (void)httpGet(obsPort, "front.local", "/page");  // HIT

        {
            std::lock_guard<std::mutex> guard(logMutex);
            check(logResults.size() == 2, "each request produced an access-log entry");
            check(logResults.size() == 2 && logResults[0] == "MISS",
                  "first request logged as a MISS");
            check(logResults.size() == 2 && logResults[1] == "HIT",
                  "second request logged as a HIT");
        }

        const auto stats = httpRaw(
            obsAdmin, "GET /stats HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
        check(contains(stats, "requests=2"), "stats counts total requests");
        check(contains(stats, "hits=1"), "stats counts cache hits");
        check(contains(stats, "misses=1"), "stats counts cache misses");
        obsEdge.stop();
    }

    // Client-side TLS termination: a separate TLS edge in front of the same
    // origin serves an HTTPS request end to end.
    {
        ruvia::edge::EdgeServerOptions tlsOptions;
        tlsOptions.tls = ruvia::edge::EdgeTlsConfig{
            std::string(edge_test_tls::kCertPem), std::string(edge_test_tls::kKeyPem)};
        tlsOptions.adminEndpoint = tcp::endpoint(tcp::v4(), 0);
        EdgeServer tlsEdge(tcp::endpoint(tcp::v4(), 0), std::move(tlsOptions));
        tlsEdge.start();
        tlsEdge.addOrigin("front.local",
                          OriginSettings{"127.0.0.1", origin.port(), false});
        const std::uint16_t tlsPort = tlsEdge.localEndpoint().port();
        const std::uint16_t tlsAdmin = tlsEdge.localAdminEndpoint().value().port();

        const auto r = httpsGet(tlsPort, "front.local", "/page");
        check(statusOf(r) == 200, "TLS-terminated request served with 200");
        check(bodyOf(r) == "hello", "TLS request proxied to the origin");

        // Rotate the certificate over the admin API; HTTPS keeps working.
        const std::string pem =
            std::string(edge_test_tls::kCertPem) + std::string(edge_test_tls::kKeyPem);
        const auto rotate = httpRaw(
            tlsAdmin, "PUT /tls HTTP/1.1\r\nHost: a\r\nContent-Length: " +
                          std::to_string(pem.size()) + "\r\nConnection: close\r\n\r\n" + pem);
        check(statusOf(rotate) == 200, "PUT /tls rotates the certificate");
        const auto afterRotate = httpsGet(tlsPort, "front.local", "/page");
        check(statusOf(afterRotate) == 200, "HTTPS still works after rotation");

        // Invalid PEM is rejected.
        const std::string junk = "not a certificate";
        const auto bad = httpRaw(
            tlsAdmin, "PUT /tls HTTP/1.1\r\nHost: a\r\nContent-Length: " +
                          std::to_string(junk.size()) + "\r\nConnection: close\r\n\r\n" + junk);
        check(statusOf(bad) == 400, "PUT /tls rejects invalid PEM");
        tlsEdge.stop();

        // A plaintext edge has no certificate to rotate.
        const auto onPlaintext = httpRaw(
            adminPort, "PUT /tls HTTP/1.1\r\nHost: a\r\nContent-Length: " +
                           std::to_string(pem.size()) + "\r\nConnection: close\r\n\r\n" + pem);
        check(statusOf(onPlaintext) == 400, "PUT /tls fails when TLS is not enabled");
    }

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
        check(origin.notModified() == notModifiedBefore + 1,
              "origin answered the conditional with 304");
        check(origin.hits() == fullBefore, "revalidation sent no full response");

        const auto c = httpGet(edgePort, "front.local", "/rev");
        check(contains(c, "X-Cache: HIT"), "refreshed entry is a hit again");
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
