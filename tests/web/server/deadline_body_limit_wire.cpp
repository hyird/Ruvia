// The three things the in-memory facade cannot reach, over a real socket:
//   - a route-declared BodyLimit actually REJECTS an oversized body (TestApp
//     dispatches in memory and never runs the server layer that enforces it);
//   - a route-declared Deadline actually cuts a handler's wait short;
//   - App::deadline()'s app-wide value reaches a route that declares none.
//
// Each was previously covered only up to "the value reached the route table",
// which is exactly the half that a silently uninitialized descriptor field once
// passed while the feature did nothing.

#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/core/Timer.h"
#include "ruvia/web/BodyLimit.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Deadline.h"
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"
#include "ruvia/web/detail/util/CallableRef.h"

using namespace std::chrono_literals;

namespace {

// Long enough that a wait which is NOT cut short cannot be mistaken for one
// that was: the deadlines below are two orders of magnitude shorter.
constexpr auto kHandlerWait = 30s;

template <typename Handler>
void registerRoute(ruvia::detail::RouterImpl& router, ruvia::HttpKnownMethod method,
    std::string_view path, Handler& handler,
    std::span<const ruvia::detail::ControllerMiddlewareDescriptor> middlewares) {
    router.registerRoute(method, std::pmr::string(path, std::pmr::get_default_resource()),
        ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(handler),
        ruvia::detail::RequestBodyMode::kBuffered, {}, middlewares);
}

[[nodiscard]] std::string readStatusLine(asio::ip::tcp::socket& socket, std::error_code& ec) {
    asio::streambuf buffer;
    asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    const std::string head(asio::buffers_begin(buffer.data()), asio::buffers_end(buffer.data()));
    const auto lineEnd = head.find("\r\n");
    return head.substr(0, lineEnd == std::string::npos ? head.size() : lineEnd);
}

// Sends one request and returns its status line, plus how long the exchange
// took -- the duration is what separates "the deadline fired" from "the handler
// simply finished".
[[nodiscard]] std::string exchange(const asio::ip::tcp::endpoint& endpoint,
    std::string_view request, std::chrono::steady_clock::duration& elapsed) {
    asio::io_context ctx;
    asio::ip::tcp::socket socket(ctx);
    std::error_code ec;
    socket.connect(endpoint, ec);
    if (ec) {
        return "connect failed";
    }
    const auto started = std::chrono::steady_clock::now();
    asio::write(socket, asio::buffer(request), ec);
    if (ec) {
        return "write failed";
    }
    auto status = readStatusLine(socket, ec);
    elapsed = std::chrono::steady_clock::now() - started;
    if (ec && status.empty()) {
        return "read failed";
    }
    return status;
}

}  // namespace

int main() {
    ruvia::detail::Router router;
    auto& routerImpl = ruvia::detail::RouterImpl::from(router);

    auto echo = [](ruvia::Context& c) -> ruvia::Task<ruvia::HttpResponse> {
        const auto body = co_await c.req().text();
        co_return c.body(body);
    };
    // Waits far longer than any deadline here. Reports which way it ended, so a
    // 504 proves the wait was cut short rather than that it ran to completion.
    auto slow = [](ruvia::Context& c) -> ruvia::Task<ruvia::HttpResponse> {
        const auto result = co_await ruvia::sleepFor(c.worker(), kHandlerWait, c.stopToken());
        if (result == ruvia::TimerSleepResult::kStopRequested) {
            c.status(ruvia::http_status::kGatewayTimeout);
            co_return c.text(c.deadlineExceeded() ? "deadline" : "shutdown");
        }
        co_return c.text("slept the whole way");
    };

    const auto smallBody = ruvia::detail::makeMiddlewareDescriptor<ruvia::BodyLimit<16>>();
    const auto shortDeadline = ruvia::detail::makeMiddlewareDescriptor<ruvia::Deadline<150>>();
    registerRoute(routerImpl, ruvia::HttpKnownMethod::kPost, "/small", echo,
        std::span(&smallBody, std::size_t{1}));
    registerRoute(routerImpl, ruvia::HttpKnownMethod::kGet, "/route-deadline", slow,
        std::span(&shortDeadline, std::size_t{1}));
    registerRoute(routerImpl, ruvia::HttpKnownMethod::kGet, "/app-deadline", slow, {});
    routerImpl.finalize();

    ruvia::detail::HttpServerOptions options;
    // Declares no route deadline, so answering in time proves the app-wide value
    // reached a route that never mentioned one.
    options.deadline = ruvia::DeadlineConfig{.handler = 200ms};

    ruvia::detail::WebWorkerRuntime server(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routerImpl.routeTable(),
        {}, options);
    server.start();
    const auto endpoint = server.localEndpoint();

    int result = 0;
    std::chrono::steady_clock::duration elapsed{};

    {
        // Within the route's ceiling.
        const auto ok = exchange(
            endpoint, "POST /small HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello", elapsed);
        if (!ok.starts_with("HTTP/1.1 200")) {
            std::fprintf(stderr, "in-limit body was not accepted: %s\n", ok.c_str());
            result = 1;
        }
    }

    if (result == 0) {
        // Over it: the server layer must reject before the handler ever runs.
        const auto tooLarge = exchange(endpoint,
            "POST /small HTTP/1.1\r\nHost: x\r\nContent-Length: 64\r\n\r\n" + std::string(64, 'x'),
            elapsed);
        if (!tooLarge.starts_with("HTTP/1.1 413")) {
            std::fprintf(stderr, "route body limit did not reject an oversized body: %s\n",
                tooLarge.c_str());
            result = 2;
        }
    }

    if (result == 0) {
        const auto routeDeadline =
            exchange(endpoint, "GET /route-deadline HTTP/1.1\r\nHost: x\r\n\r\n", elapsed);
        if (!routeDeadline.starts_with("HTTP/1.1 504")) {
            std::fprintf(stderr, "route deadline did not cut the handler's wait: %s\n",
                routeDeadline.c_str());
            result = 3;
        } else if (elapsed > kHandlerWait / 2) {
            std::fprintf(stderr, "route deadline answered only after the full wait\n");
            result = 4;
        }
    }

    if (result == 0) {
        const auto appDeadline =
            exchange(endpoint, "GET /app-deadline HTTP/1.1\r\nHost: x\r\n\r\n", elapsed);
        if (!appDeadline.starts_with("HTTP/1.1 504")) {
            std::fprintf(stderr, "app-wide deadline did not reach a route without its own: %s\n",
                appDeadline.c_str());
            result = 5;
        } else if (elapsed > kHandlerWait / 2) {
            std::fprintf(stderr, "app-wide deadline answered only after the full wait\n");
            result = 6;
        }
    }

    server.stop();
    server.join();
    return result;
}

