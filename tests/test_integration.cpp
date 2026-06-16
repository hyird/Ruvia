#include <doctest/doctest.h>

// Access the internal HttpServer and router directly
#include "net/HttpServer.h"
#include "router/RouterInternal.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/Error.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"

#include <asio/ip/tcp.hpp>
#include <asio/io_context.hpp>
#include <asio/connect.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>
#include <asio/streambuf.hpp>
#include <chrono>
#include <string>
#include <thread>

using namespace ruvia;
using namespace ruvia::detail;

namespace {

// ─── Minimal HTTP/1.1 client for testing ──────────────────────────────────

struct TestResponse {
    int statusCode{0};
    std::string statusText;
    std::string body;
    std::string contentType;
    std::string connection;
};

TestResponse sendRequest(
    std::uint16_t port,
    std::string_view method,
    std::string_view path,
    std::string_view extraHeaders = {},
    std::string_view body = {}) {
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    asio::ip::tcp::resolver resolver(io);
    asio::connect(sock, resolver.resolve("127.0.0.1", std::to_string(port)));

    std::string request;
    request += method;
    request += ' ';
    request += path;
    request += " HTTP/1.1\r\n";
    request += "Host: localhost\r\n";
    if (!body.empty()) {
        request += "Content-Length: ";
        request += std::to_string(body.size());
        request += "\r\n";
    }
    request += extraHeaders;
    request += "\r\n";
    request += body;

    asio::write(sock, asio::buffer(request));

    asio::streambuf buf;
    std::string headers;
    std::size_t headersEnd = 0;

    // Read until \r\n\r\n
    std::string raw;
    raw.resize(8192);
    std::error_code ec;
    std::size_t n = sock.read_some(asio::buffer(raw), ec);
    raw.resize(n);

    TestResponse resp;
    const auto h = raw.find("\r\n\r\n");
    if (h == std::string::npos) return resp;

    const auto headersStr = raw.substr(0, h);
    const auto bodyStart = h + 4;

    // Parse status line
    const auto sp1 = headersStr.find(' ');
    const auto sp2 = headersStr.find(' ', sp1 + 1);
    const auto nl = headersStr.find("\r\n");
    if (sp1 == std::string::npos || sp2 == std::string::npos) return resp;
    resp.statusCode = std::stoi(headersStr.substr(sp1 + 1, sp2 - sp1 - 1));
    resp.statusText = headersStr.substr(sp2 + 1, nl - sp2 - 1);
    resp.body = raw.substr(bodyStart);

    // Parse headers
    std::size_t pos = nl + 2;
    while (pos < headersStr.size()) {
        const auto lineEnd = headersStr.find("\r\n", pos);
        const auto line = lineEnd == std::string::npos
            ? headersStr.substr(pos)
            : headersStr.substr(pos, lineEnd - pos);
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            auto name = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value = value.substr(1);
            if (name == "Content-Type") resp.contentType = value;
            if (name == "Connection") resp.connection = value;
        }
        if (lineEnd == std::string::npos) break;
        pos = lineEnd + 2;
    }
    return resp;
}

// ─── Minimal server builder ────────────────────────────────────────────────

struct TestServer {
    Router router;
    std::unique_ptr<HttpServer> server;
    std::uint16_t port{0};

    ~TestServer() {
        if (server) {
            server->stop();
            server->join();
        }
    }
};

}  // namespace

// ─── Integration tests ─────────────────────────────────────────────────────

TEST_SUITE("Integration / HTTP server basics") {

TEST_CASE("GET /ping returns 200") {
    Router router;
    auto& impl = RouterImpl::from(router);

    impl.registerRoute(
        HttpMethod::kGet,
        std::pmr::string("/ping", ProcessMemory::instance().upstreamResource()),
        RouteHandler{
            nullptr,
            [](void*, Context& ctx) -> Task<HttpResponse> {
                co_return ctx.text("pong");
            }
        },
        RequestBodyMode::kBuffered);
    impl.finalize();

    const auto& table = impl.routeTable();
    auto endpoint = asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0);
    HttpServerOptions opts;
    opts.compression.enabled = false;

    HttpServer srv(endpoint, table, {}, opts);
    srv.start();
    const auto port = srv.localEndpoint().port();

    auto resp = sendRequest(port, "GET", "/ping");
    CHECK(resp.statusCode == 200);
    CHECK(resp.body == "pong");

    srv.stop();
    srv.join();
}

TEST_CASE("GET unknown path returns 404") {
    Router router;
    auto& impl = RouterImpl::from(router);

    impl.registerRoute(
        HttpMethod::kGet,
        std::pmr::string("/exists", ProcessMemory::instance().upstreamResource()),
        RouteHandler{nullptr, [](void*, Context& ctx) -> Task<HttpResponse> {
            co_return ctx.text("ok");
        }},
        RequestBodyMode::kBuffered);
    impl.finalize();

    const auto& table = impl.routeTable();
    auto endpoint = asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0);
    HttpServerOptions opts;
    opts.compression.enabled = false;

    HttpServer srv(endpoint, table, {}, opts);
    srv.start();
    const auto port = srv.localEndpoint().port();

    auto resp = sendRequest(port, "GET", "/missing");
    CHECK(resp.statusCode == 404);

    srv.stop();
    srv.join();
}

TEST_CASE("POST to GET route returns 405 with Allow header") {
    Router router;
    auto& impl = RouterImpl::from(router);

    impl.registerRoute(
        HttpMethod::kGet,
        std::pmr::string("/items", ProcessMemory::instance().upstreamResource()),
        RouteHandler{nullptr, [](void*, Context& ctx) -> Task<HttpResponse> {
            co_return ctx.text("[]");
        }},
        RequestBodyMode::kBuffered);
    impl.finalize();

    const auto& table = impl.routeTable();
    auto endpoint = asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0);
    HttpServerOptions opts;
    opts.compression.enabled = false;

    HttpServer srv(endpoint, table, {}, opts);
    srv.start();
    const auto port = srv.localEndpoint().port();

    auto resp = sendRequest(port, "POST", "/items", "", "{}");
    CHECK(resp.statusCode == 405);

    srv.stop();
    srv.join();
}

TEST_CASE("HEAD request returns 200 with no body") {
    Router router;
    auto& impl = RouterImpl::from(router);

    impl.registerRoute(
        HttpMethod::kGet,
        std::pmr::string("/data", ProcessMemory::instance().upstreamResource()),
        RouteHandler{nullptr, [](void*, Context& ctx) -> Task<HttpResponse> {
            co_return ctx.text("hello world");
        }},
        RequestBodyMode::kBuffered);
    impl.finalize();

    const auto& table = impl.routeTable();
    auto endpoint = asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0);
    HttpServerOptions opts;
    opts.compression.enabled = false;

    HttpServer srv(endpoint, table, {}, opts);
    srv.start();
    const auto port = srv.localEndpoint().port();

    auto resp = sendRequest(port, "HEAD", "/data");
    CHECK(resp.statusCode == 200);
    CHECK(resp.body.empty());

    srv.stop();
    srv.join();
}

TEST_CASE("handler throwing HttpError returns correct status") {
    Router router;
    auto& impl = RouterImpl::from(router);

    impl.registerRoute(
        HttpMethod::kGet,
        std::pmr::string("/secure", ProcessMemory::instance().upstreamResource()),
        RouteHandler{nullptr, [](void*, Context&) -> Task<HttpResponse> {
            throw HttpError(401, "unauthorized", "login required");
            co_return HttpResponse(std::pmr::get_default_resource());
        }},
        RequestBodyMode::kBuffered);
    impl.finalize();

    const auto& table = impl.routeTable();
    auto endpoint = asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0);
    HttpServerOptions opts;
    opts.compression.enabled = false;

    HttpServer srv(endpoint, table, {}, opts);
    srv.start();
    const auto port = srv.localEndpoint().port();

    auto resp = sendRequest(port, "GET", "/secure");
    CHECK(resp.statusCode == 401);

    srv.stop();
    srv.join();
}

TEST_CASE("JSON response has correct Content-Type") {
    Router router;
    auto& impl = RouterImpl::from(router);

    impl.registerRoute(
        HttpMethod::kGet,
        std::pmr::string("/json", ProcessMemory::instance().upstreamResource()),
        RouteHandler{nullptr, [](void*, Context& ctx) -> Task<HttpResponse> {
            co_return ctx.json(42);
        }},
        RequestBodyMode::kBuffered);
    impl.finalize();

    const auto& table = impl.routeTable();
    auto endpoint = asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0);
    HttpServerOptions opts;
    opts.compression.enabled = false;

    HttpServer srv(endpoint, table, {}, opts);
    srv.start();
    const auto port = srv.localEndpoint().port();

    auto resp = sendRequest(port, "GET", "/json");
    CHECK(resp.statusCode == 200);
    CHECK(resp.contentType.find("application/json") != std::string::npos);
    CHECK(resp.body.find("42") != std::string::npos);

    srv.stop();
    srv.join();
}

}  // TEST_SUITE
