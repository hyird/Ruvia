// Worker-local user state: app-registered state must materialize once per
// worker, persist across requests, connections, and listeners on that worker,
// be the same instance the WebWorker dispatch path sees, fail loudly for an
// unregistered type, and be destroyed with the worker. Drives one real worker
// with two independently bound HTTP/1.1 listeners.

#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/WebWorker.h"
#include "ruvia/web/detail/integration/WorkerState.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

namespace {

bool g_probeDestroyed = false;
bool g_probeFactorySawWorkerIdentity = false;
bool g_probeDestroySawWorkerIdentity = false;
std::thread::id g_probeFactoryThread;
std::thread::id g_probeDestroyThread;

struct ProbeState final {
    explicit ProbeState(ruvia::WorkerHandle owner)
        : worker(std::move(owner)),
          counter(0) {
        g_probeFactoryThread = std::this_thread::get_id();
        g_probeFactorySawWorkerIdentity = worker.isCurrent();
    }

    ruvia::WorkerHandle worker;
    int counter{0};
    // Only the worker's live instance ever reaches 14; the factory's moved-from
    // temporaries are destroyed at 0 and must not satisfy the teardown check.
    ~ProbeState() {
        if (counter >= 14) {
            g_probeDestroyed = true;
            g_probeDestroyThread = std::this_thread::get_id();
            g_probeDestroySawWorkerIdentity = worker.isCurrent();
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
    body.append("listener:");
    body.append(std::to_string(ruvia::getConnInfo(context).listener()->value()));
    body.append(";count:");
    body.append(std::to_string(state.counter));
    co_return context.body(std::move(body));
}

ruvia::Task<ruvia::HttpResponse> missingStateHandler(void*, ruvia::Context& context) {
    (void)context.workerState<UnregisteredState>();
    co_return context.body("unreachable");
}

[[nodiscard]] std::string readResponse(asio::ip::tcp::socket& socket, asio::streambuf& buffer, std::error_code& ec) {
    const std::size_t n = asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    std::string head(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + n);
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
    std::string body(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + std::min(buffer.size(), length));
    buffer.consume(std::min(buffer.size(), length));
    return head + body;
}

}  // namespace

int main() {
    const auto callerThread = std::this_thread::get_id();
    int rc = 0;
    auto fail = [&](int code, const char* message) {
        std::fputs(message, stderr);
        std::fputc('\n', stderr);
        rc = code;
    };

    {
        ruvia::detail::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        std::pmr::string countPath("/count", std::pmr::get_default_resource());
        std::pmr::string missingPath("/missing", std::pmr::get_default_resource());
        impl.registerRoute(ruvia::HttpKnownMethod::kGet, std::move(countPath), ruvia::detail::RouteHandler(nullptr, &countHandler), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
        impl.registerRoute(ruvia::HttpKnownMethod::kGet, std::move(missingPath), ruvia::detail::RouteHandler(nullptr, &missingStateHandler), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
        impl.finalize();

        ruvia::WorkerHandle worker;
        ruvia::detail::WorkerStateDefinition workerStates[] = {
            ruvia::detail::WorkerStateDefinition::make<ProbeState>([&worker] { return ProbeState(worker); }),
        };

        const auto loopback = asio::ip::make_address("127.0.0.1");
        ruvia::detail::HttpServerListenerDefinition listeners[] = {
            {ruvia::ListenerId{1}, asio::ip::tcp::endpoint(loopback, 0)},
            {ruvia::ListenerId{2}, asio::ip::tcp::endpoint(loopback, 0)},
        };
        ruvia::detail::HttpServerOptions options;
        ruvia::detail::WebWorkerRuntime server(
            std::span<const ruvia::detail::HttpServerListenerDefinition>(listeners),
            impl.routeTable(),
            {.workerStates = std::span<const ruvia::detail::WorkerStateDefinition>(workerStates, 1)},
            options);
        worker = server.worker();
        server.start();
        const auto firstEndpoint = server.localEndpoint(ruvia::ListenerId{1});
        const auto secondEndpoint = server.localEndpoint(ruvia::ListenerId{2});
        if (firstEndpoint == secondEndpoint) {
            fail(10, "independent listeners resolved to the same endpoint");
        }

        asio::io_context ctx;
        std::error_code ec;

        // Two requests on one connection, then one on a fresh connection: the
        // state is per WORKER, not per request or per connection.
        {
            asio::ip::tcp::socket sock(ctx);
            sock.connect(firstEndpoint, ec);
            asio::streambuf buffer;
            asio::write(sock, asio::buffer(std::string_view("GET /count HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
            if (readResponse(sock, buffer, ec).find("listener:1;count:1") == std::string::npos) {
                fail(1, "first request did not see a fresh worker state");
            }
            asio::write(sock, asio::buffer(std::string_view("GET /count HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
            if (rc == 0 && readResponse(sock, buffer, ec).find("listener:1;count:2") == std::string::npos) {
                fail(2, "second request did not see the first request's mutation");
            }
            sock.close(ec);
        }
        if (rc == 0) {
            asio::ip::tcp::socket sock(ctx);
            sock.connect(secondEndpoint, ec);
            asio::streambuf buffer;
            asio::write(sock, asio::buffer(std::string_view("GET /count HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
            if (readResponse(sock, buffer, ec).find("listener:2;count:3") == std::string::npos) {
                fail(3, "the second listener did not share the worker-scoped state");
            }

            // The WebWorker dispatch path shares the same instance.
            if (rc == 0) {
                const auto post = server.webWorker().post([](ruvia::WebWorkerContext& workerContext) -> ruvia::Task<void> {
                    workerContext.workerState<ProbeState>().counter += 10;
                    co_return;
                });
                if (post != ruvia::PostStatus::kAccepted) {
                    fail(4, "worker dispatch rejected the state mutation task");
                }
                for (int i = 0; rc == 0 && i < 200; ++i) {
                    if (server.webWorker().stats().completed >= 1) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                asio::write(sock, asio::buffer(std::string_view("GET /count HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
                if (rc == 0 && readResponse(sock, buffer, ec).find("listener:2;count:14") == std::string::npos) {
                    fail(5, "HTTP and dispatch paths did not share one instance");
                }
            }

            // An unregistered type is a loud 500, not a silent default.
            if (rc == 0) {
                asio::write(sock, asio::buffer(std::string_view("GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n")), ec);
                const auto response = readResponse(sock, buffer, ec);
                if (!response.starts_with("HTTP/1.1 500")) {
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
    if (rc == 0 && (g_probeFactoryThread == callerThread || g_probeDestroyThread != g_probeFactoryThread)) {
        fail(8, "worker state construction/destruction was not worker-affine");
    }
    if (rc == 0 && (!g_probeFactorySawWorkerIdentity || !g_probeDestroySawWorkerIdentity)) {
        fail(9, "worker state lifetime ran outside the worker identity window");
    }
    return rc;
}
