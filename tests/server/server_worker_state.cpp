// Worker-local user state: app-registered state must materialize once per
// worker, persist across requests AND connections on that worker, be the same
// instance the WebWorker dispatch path sees, fail loudly for an unregistered
// type, and be destroyed with the worker. Drives one real single-worker
// HTTP/1.1 loopback server.

#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <string>
#include <string_view>
#include <thread>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/web/Context.h"
#include "ruvia/web/Router.h"
#include "ruvia/web/WebWorker.h"
#include "ruvia/web/detail/WorkerState.h"
#include "ruvia/web/detail/router/RouterInternal.h"
#include "ruvia/web/detail/server/HttpServer.h"

namespace {

bool g_probeDestroyed = false;

struct ProbeState final {
    int counter{0};
    // Only the worker's live instance ever reaches 14; the factory's moved-from
    // temporaries are destroyed at 0 and must not satisfy the teardown check.
    ~ProbeState() {
        if (counter >= 14) {
            g_probeDestroyed = true;
        }
    }
};

struct UnregisteredState final {
    int unused{0};
};

ruvia::Task<ruvia::HttpResponse> countHandler(void*, ruvia::Context& context) {
    auto& state = context.workerState<ProbeState>();
    ++state.counter;
    std::pmr::string body(context.resource());
    body.append("count:");
    body.append(std::to_string(state.counter));
    co_return context.body(std::move(body));
}

ruvia::Task<ruvia::HttpResponse> missingStateHandler(void*, ruvia::Context& context) {
    (void)context.workerState<UnregisteredState>();
    co_return context.body("unreachable");
}

[[nodiscard]] std::string readResponse(
    asio::ip::tcp::socket& socket, asio::streambuf& buffer, std::error_code& ec) {
    const std::size_t n = asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    std::string head(
        asio::buffers_begin(buffer.data()),
        asio::buffers_begin(buffer.data()) + n);
    buffer.consume(n);

    std::size_t length = 0;
    for (std::string_view rest = head; !rest.empty();) {
        const auto eol = rest.find("\r\n");
        const auto line = rest.substr(0, eol);
        constexpr std::string_view name = "content-length:";
        if (line.size() > name.size()) {
            bool match = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(line[i])) != name[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                for (char c : line.substr(name.size())) {
                    if (c >= '0' && c <= '9') {
                        length = length * 10 + static_cast<std::size_t>(c - '0');
                    }
                }
            }
        }
        if (eol == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(eol + 2);
    }

    while (buffer.size() < length && !ec) {
        asio::read_until(socket, buffer, "\n", ec);
        if (buffer.size() >= length) {
            break;
        }
    }
    std::string body(
        asio::buffers_begin(buffer.data()),
        asio::buffers_begin(buffer.data()) + std::min(buffer.size(), length));
    buffer.consume(std::min(buffer.size(), length));
    return head + body;
}

}  // namespace

int main() {
    int rc = 0;
    auto fail = [&](int code, const char* message) {
        std::fputs(message, stderr);
        std::fputc('\n', stderr);
        rc = code;
    };

    {
        ruvia::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        std::pmr::string countPath("/count", std::pmr::get_default_resource());
        std::pmr::string missingPath("/missing", std::pmr::get_default_resource());
        impl.registerRoute(
            ruvia::HttpKnownMethod::kGet, std::move(countPath),
            ruvia::detail::RouteHandler(nullptr, &countHandler),
            ruvia::detail::RequestBodyMode::kBuffered, {}, {});
        impl.registerRoute(
            ruvia::HttpKnownMethod::kGet, std::move(missingPath),
            ruvia::detail::RouteHandler(nullptr, &missingStateHandler),
            ruvia::detail::RequestBodyMode::kBuffered, {}, {});
        impl.finalize();

        ruvia::detail::WorkerStateDefinition workerStates[] = {
            ruvia::detail::WorkerStateDefinition::make<ProbeState>(
                [] { return ProbeState{}; }),
        };

        ruvia::detail::HttpServerOptions options;
        options.shutdownGracePeriod = std::chrono::milliseconds(0);
        ruvia::detail::HttpServer server(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
            impl.routeTable(), {}, {},
            std::span<const ruvia::detail::WorkerStateDefinition>(workerStates, 1),
            options);
        server.start();
        const auto endpoint = server.localEndpoint();

        asio::io_context ctx;
        std::error_code ec;

        // Two requests on one connection, then one on a fresh connection: the
        // state is per WORKER, not per request or per connection.
        {
            asio::ip::tcp::socket sock(ctx);
            sock.connect(endpoint, ec);
            asio::streambuf buffer;
            asio::write(sock, asio::buffer(std::string_view(
                "GET /count HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
            if (readResponse(sock, buffer, ec).find("count:1") == std::string::npos) {
                fail(1, "first request did not see a fresh worker state");
            }
            asio::write(sock, asio::buffer(std::string_view(
                "GET /count HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
            if (rc == 0 &&
                readResponse(sock, buffer, ec).find("count:2") == std::string::npos) {
                fail(2, "second request did not see the first request's mutation");
            }
            sock.close(ec);
        }
        if (rc == 0) {
            asio::ip::tcp::socket sock(ctx);
            sock.connect(endpoint, ec);
            asio::streambuf buffer;
            asio::write(sock, asio::buffer(std::string_view(
                "GET /count HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
            if (readResponse(sock, buffer, ec).find("count:3") == std::string::npos) {
                fail(3, "a new connection did not see the worker-scoped state");
            }

            // The WebWorker dispatch path shares the same instance.
            if (rc == 0) {
                const auto post = server.webWorker().post(
                    [](ruvia::WebWorkerContext& worker) -> ruvia::Task<void> {
                        worker.workerState<ProbeState>().counter += 10;
                        co_return;
                    });
                if (post != ruvia::PostResult::kAccepted) {
                    fail(4, "worker dispatch rejected the state mutation task");
                }
                for (int i = 0; rc == 0 && i < 200; ++i) {
                    if (server.webWorker().stats().completed >= 1) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                asio::write(sock, asio::buffer(std::string_view(
                    "GET /count HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
                if (rc == 0 &&
                    readResponse(sock, buffer, ec).find("count:14") == std::string::npos) {
                    fail(5, "HTTP and dispatch paths did not share one instance");
                }
            }

            // An unregistered type is a loud 500, not a silent default.
            if (rc == 0) {
                asio::write(sock, asio::buffer(std::string_view(
                    "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
                const auto response = readResponse(sock, buffer, ec);
                if (response.rfind("HTTP/1.1 500", 0) != 0) {
                    fail(6, "unregistered worker state did not fail with 500");
                }
            }
            sock.close(ec);
        }

        server.stop();
        server.join();
    }

    if (rc == 0 && !g_probeDestroyed) {
        fail(7, "worker state instance was not destroyed with the worker");
    }
    return rc;
}
