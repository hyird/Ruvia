// OriginFetcher opens a fresh plaintext HTTP/1.1 connection to an origin, sends
// the request, and materializes the response. These checks drive it against a
// loopback origin that replies with each of the four body framings the MVP
// handles -- exact length, chunked, close-delimited, and no-content -- plus a
// check that the request the origin receives is well-formed (request line and a
// writer-generated Host header).

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
#include <asio/write.hpp>
#include <asio/as_tuple.hpp>
#include <asio/use_awaitable.hpp>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/edge/OriginFetcher.h"
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

struct CaseResult final {
    OriginFetchOutcome outcome{OriginFetchOutcome::kConnectFailed};
    std::uint16_t status{0};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string requestSeenByOrigin;
};

// Run one fetch against a loopback origin that replies with `responseBytes`.
CaseResult runCase(std::string responseBytes) {
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
            const OriginFetcher fetcher(OriginFetcher::Limits{});
            const HttpHeaderView headers[] = {HttpHeaderView("accept", "*/*")};
            OriginRequest request;
            request.method = "GET";
            request.target = "/thing";
            request.headers = std::span<const HttpHeaderView>(headers);
            auto result = co_await ruvia::detail::taskAsAwaitable(
                fetcher.fetch(io.get_executor(), "127.0.0.1", port, request));
            out.outcome = result.outcome;
            out.status = result.response.status;
            out.body = std::move(result.response.body);
            out.headers = std::move(result.response.headers);
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

    if (failures == 0) {
        std::fprintf(stderr, "origin fetcher: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
