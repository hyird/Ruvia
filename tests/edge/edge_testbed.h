#pragma once

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

namespace edge_testbed {

inline int failures = 0;

inline void check(bool condition, const char* message) {
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
inline std::string httpRaw(std::uint16_t port, const std::string& request) {
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

inline std::string httpGet(std::uint16_t port, std::string_view host, std::string_view target) {
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

inline std::string httpHead(std::uint16_t port, std::string_view host, std::string_view target) {
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
inline std::string httpGetRange(
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
inline std::string httpGetEnc(
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

inline std::string httpPost(
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
inline std::string readOneResponse(asio::ip::tcp::socket& socket) {
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
inline std::pair<std::string, std::string> httpKeepAliveTwo(
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
inline std::string httpsGet(std::uint16_t port, std::string_view host, std::string_view target) {
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
inline std::string runShell(const std::string& command) {
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

}  // namespace edge_testbed

using namespace edge_testbed;  // NOLINT(google-build-using-namespace)
