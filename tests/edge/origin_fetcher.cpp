// OriginFetcher opens a fresh plaintext HTTP/1.1 connection to an origin, sends
// the request, and materializes the response. These checks drive it against a
// loopback origin that replies with each of the four body framings the MVP
// handles -- exact length, chunked, close-delimited, and no-content -- plus a
// check that the request the origin receives is well-formed (request line and a
// writer-generated Host header).

#include <chrono>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>
#include <asio/as_tuple.hpp>
#include <asio/use_awaitable.hpp>

#include "tls_fixture.h"
#include "ruvia/edge/detail/OriginFetcher.h"
#include "ruvia/http/HttpHeader.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

using asio::ip::tcp;
using ruvia::HttpHeaderView;
using ruvia::edge::OriginFetcher;
using ruvia::edge::OriginFetchOutcome;
using ruvia::edge::OriginRequest;
using ruvia::edge::OriginResponseHead;
using ruvia::edge::ResponseSink;

// A sink that accumulates the streamed head and body so a test can inspect them.
struct AccumulatingSink final {
    OriginResponseHead head;
    std::string body;

    [[nodiscard]] ResponseSink make() {
        ResponseSink sink;
        sink.onHead = [this](const OriginResponseHead& h) -> asio::awaitable<bool> {
            head = h;
            co_return true;
        };
        sink.onBody = [this](std::string_view chunk) -> asio::awaitable<bool> {
            body.append(chunk);
            co_return true;
        };
        return sink;
    }
};

struct CaseResult final {
    OriginFetchOutcome outcome{OriginFetchOutcome::kConnectFailed};
    std::uint16_t status{0};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string requestSeenByOrigin;
};

// Run one fetch against a loopback origin. When `silent`, the origin accepts and
// reads the request but never replies, holding the connection open so the fetch
// hits its read deadline.
CaseResult runCase(
    std::string responseBytes,
    bool silent = false,
    OriginFetcher::Limits limits = {}) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    CaseResult out;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto [acceptEc, socket] =
                co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
            if (acceptEc) {
                co_return;
            }
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
            out.requestSeenByOrigin = request;
            if (silent) {
                // Hold the connection open without responding.
                asio::steady_timer hold(io);
                hold.expires_after(std::chrono::seconds(5));
                co_await hold.async_wait(asio::as_tuple(asio::use_awaitable));
                co_return;
            }
            co_await asio::async_write(
                socket,
                asio::buffer(responseBytes),
                asio::as_tuple(asio::use_awaitable));
            asio::error_code ignore;
            socket.shutdown(tcp::socket::shutdown_send, ignore);
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            OriginFetcher fetcher(limits);
            const HttpHeaderView headers[] = {HttpHeaderView("accept", "*/*")};
            OriginRequest request;
            request.method = "GET";
            request.target = "/thing";
            request.headers = std::span<const HttpHeaderView>(headers);
            AccumulatingSink acc;
            auto sink = acc.make();
            auto result = co_await fetcher.fetch(
                io.get_executor(), "127.0.0.1", port, false, request, sink);
            out.outcome = result.outcome;
            out.status = acc.head.status;
            out.body = std::move(acc.body);
            out.headers = std::move(acc.head.headers);
            io.stop();  // done measuring; do not wait on a held-open origin
            co_return;
        },
        asio::detached);

    io.run();
    return out;
}

[[nodiscard]] bool hasHeader(
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::string_view name,
    std::string_view value) {
    for (const auto& [n, v] : headers) {
        if (n == name && v == value) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    // Exact Content-Length body, and a well-formed request reaching the origin.
    {
        const auto r = runCase(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 5\r\n"
            "Cache-Control: max-age=60\r\n"
            "\r\n"
            "hello");
        check(r.outcome == OriginFetchOutcome::kOk, "known-length fetch succeeds");
        check(r.status == 200, "known-length status is 200");
        check(r.body == "hello", "known-length body decoded");
        check(hasHeader(r.headers, "Cache-Control", "max-age=60"),
              "response headers are captured verbatim");
        check(r.requestSeenByOrigin.starts_with("GET /thing HTTP/1.1\r\n"),
              "origin sees the request line");
        check(r.requestSeenByOrigin.find("Host: 127.0.0.1:") != std::string::npos,
              "writer generated a Host header");
        check(r.requestSeenByOrigin.find("accept: */*") != std::string::npos,
              "forwarded request header reached the origin");
    }

    // Chunked transfer-coding is de-chunked into a contiguous body.
    {
        const auto r = runCase(
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "5\r\nhello\r\n"
            "6\r\n world\r\n"
            "0\r\n\r\n");
        check(r.outcome == OriginFetchOutcome::kOk, "chunked fetch succeeds");
        check(r.status == 200, "chunked status is 200");
        check(r.body == "hello world", "chunked body reassembled");
    }

    // No Content-Length and no chunking: the body runs to connection close.
    {
        const auto r = runCase(
            "HTTP/1.1 200 OK\r\n"
            "Connection: close\r\n"
            "\r\n"
            "close-delimited-body");
        check(r.outcome == OriginFetchOutcome::kOk, "close-delimited fetch succeeds");
        check(r.body == "close-delimited-body", "close-delimited body read to EOF");
    }

    // 204 has no message body.
    {
        const auto r = runCase(
            "HTTP/1.1 204 No Content\r\n"
            "Cache-Control: max-age=60\r\n"
            "\r\n");
        check(r.outcome == OriginFetchOutcome::kOk, "no-content fetch succeeds");
        check(r.status == 204, "no-content status is 204");
        check(r.body.empty(), "no-content body is empty");
    }

    // An origin that accepts but never replies trips the read deadline.
    {
        OriginFetcher::Limits limits;
        limits.connectTimeout = std::chrono::milliseconds(200);
        limits.ioTimeout = std::chrono::milliseconds(200);
        const auto r = runCase("", /*silent=*/true, limits);
        check(r.outcome == OriginFetchOutcome::kTimeout,
              "a non-responding origin times out");
    }

    // Connection pooling: two sequential fetches to a keep-alive origin reuse one
    // TCP connection.
    {
        asio::io_context io;
        tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 0));
        const std::uint16_t port = acceptor.local_endpoint().port();
        int connectionsAccepted = 0;

        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
                for (;;) {
                    auto [aec, socket] =
                        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
                    if (aec) {
                        co_return;
                    }
                    ++connectionsAccepted;
                    // Serve keep-alive requests on this one connection.
                    asio::co_spawn(
                        io,
                        [sock = std::move(socket)]() mutable -> asio::awaitable<void> {
                            std::string request;
                            char buffer[1024];
                            for (;;) {
                                while (request.find("\r\n\r\n") == std::string::npos) {
                                    auto [ec, n] = co_await sock.async_read_some(
                                        asio::buffer(buffer),
                                        asio::as_tuple(asio::use_awaitable));
                                    if (n > 0) {
                                        request.append(buffer, n);
                                    }
                                    if (ec) {
                                        co_return;
                                    }
                                }
                                static constexpr std::string_view kResponse =
                                    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
                                co_await asio::async_write(
                                    sock, asio::buffer(kResponse),
                                    asio::as_tuple(asio::use_awaitable));
                                request.erase(0, request.find("\r\n\r\n") + 4);
                            }
                        },
                        asio::detached);
                }
            },
            asio::detached);

        OriginFetchOutcome outcome1 = OriginFetchOutcome::kConnectFailed;
        OriginFetchOutcome outcome2 = OriginFetchOutcome::kConnectFailed;
        std::size_t idleAfterFirst = 0;

        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
                OriginFetcher fetcher(OriginFetcher::Limits{});
                const HttpHeaderView headers[] = {HttpHeaderView("accept", "*/*")};
                OriginRequest request;
                request.method = "GET";
                request.headers = std::span<const HttpHeaderView>(headers);

                request.target = "/first";
                AccumulatingSink acc1;
                auto sink1 = acc1.make();
                auto r1 = co_await fetcher.fetch(
                    io.get_executor(), "127.0.0.1", port, false, request, sink1);
                outcome1 = r1.outcome;
                idleAfterFirst = fetcher.idleConnectionCount();

                request.target = "/second";
                AccumulatingSink acc2;
                auto sink2 = acc2.make();
                auto r2 = co_await fetcher.fetch(
                    io.get_executor(), "127.0.0.1", port, false, request, sink2);
                outcome2 = r2.outcome;

                io.stop();
                co_return;
            },
            asio::detached);

        io.run();
        check(outcome1 == OriginFetchOutcome::kOk, "first pooled fetch succeeds");
        check(outcome2 == OriginFetchOutcome::kOk, "second pooled fetch succeeds");
        check(idleAfterFirst == 1, "connection is pooled after the first fetch");
        check(connectionsAccepted == 1, "second fetch reused the pooled connection");
    }

    // TLS origin: fetch over an encrypted connection. Run once with verification
    // off (a self-signed origin is accepted and the body decoded) and once with
    // verification on (the self-signed / host-mismatched origin is rejected).
    {
        struct TlsFetchResult {
            OriginFetchOutcome outcome{OriginFetchOutcome::kConnectFailed};
            std::string body;
        };
        const auto runTlsFetch = [](bool verify) -> TlsFetchResult {
            asio::io_context io;
            tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 0));
            const std::uint16_t port = acceptor.local_endpoint().port();

            asio::ssl::context serverContext(asio::ssl::context::tls_server);
            serverContext.use_certificate_chain(asio::buffer(edge_test_tls::kCertPem));
            serverContext.use_private_key(
                asio::buffer(edge_test_tls::kKeyPem), asio::ssl::context::pem);

            asio::co_spawn(
                io,
                [&]() -> asio::awaitable<void> {
                    auto [aec, socket] =
                        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
                    if (aec) {
                        co_return;
                    }
                    asio::ssl::stream<tcp::socket> tls(std::move(socket), serverContext);
                    auto [hec] = co_await tls.async_handshake(
                        asio::ssl::stream_base::server, asio::as_tuple(asio::use_awaitable));
                    if (hec) {
                        co_return;  // client rejected the certificate
                    }
                    std::string request;
                    char buffer[1024];
                    while (request.find("\r\n\r\n") == std::string::npos) {
                        auto [ec, n] = co_await tls.async_read_some(
                            asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
                        if (n > 0) {
                            request.append(buffer, n);
                        }
                        if (ec) {
                            co_return;
                        }
                    }
                    static constexpr std::string_view kResponse =
                        "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsecure";
                    co_await asio::async_write(
                        tls, asio::buffer(kResponse), asio::as_tuple(asio::use_awaitable));
                    co_return;
                },
                asio::detached);

            TlsFetchResult out;
            asio::co_spawn(
                io,
                [&]() -> asio::awaitable<void> {
                    OriginFetcher::Limits limits;
                    limits.verifyOriginCertificate = verify;
                    OriginFetcher fetcher(limits);
                    const HttpHeaderView headers[] = {HttpHeaderView("accept", "*/*")};
                    OriginRequest request;
                    request.method = "GET";
                    request.target = "/secure";
                    request.headers = std::span<const HttpHeaderView>(headers);
                    AccumulatingSink acc;
                    auto sink = acc.make();
                    auto r = co_await fetcher.fetch(
                        io.get_executor(), "127.0.0.1", port, /*https=*/true, request, sink);
                    out.outcome = r.outcome;
                    out.body = std::move(acc.body);
                    io.stop();
                    co_return;
                },
                asio::detached);

            io.run();
            return out;
        };

        const auto trusting = runTlsFetch(/*verify=*/false);
        check(trusting.outcome == OriginFetchOutcome::kOk,
              "https origin fetch succeeds with verification off");
        check(trusting.body == "secure", "https origin body decoded over TLS");

        const auto verifying = runTlsFetch(/*verify=*/true);
        check(verifying.outcome == OriginFetchOutcome::kConnectFailed,
              "certificate verification rejects the untrusted origin");
    }

    if (failures == 0) {
        std::fprintf(stderr, "origin fetcher: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
