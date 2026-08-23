// Offloading blocking work: a handler that calls Context::runBlocking() must
// not stop its worker from serving other connections, must get its result (or
// its exception) back, must answer an overloaded pool with 503 rather than 500,
// must let tryRunBlocking() see that overload without throwing, and must fail
// loudly when no pool was configured. Drives real single-worker HTTP/1.1
// loopback servers.

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

namespace {

using namespace std::chrono_literals;

// Polls the pool until it reports the state the next step depends on. The pool
// is driven through a socket, so every transition is observed rather than
// awaited; a bounded poll keeps a wrong state from hanging the suite.
template <typename Predicate>
[[nodiscard]] bool waitForPool(const ruvia::BlockingPool& pool, Predicate&& predicate) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (predicate(pool.stats())) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

// Holds a pool thread until the test releases it.
std::counting_semaphore<8> g_hold{0};

ruvia::Task<ruvia::HttpResponse> holdHandler(void*, ruvia::Context& context) {
    auto held = co_await context.runBlocking([] {
        g_hold.acquire();
        return std::string("held");
    });
    co_return context.text(std::string_view(held));
}

ruvia::Task<ruvia::HttpResponse> slowHandler(void*, ruvia::Context& context) {
    auto slow = co_await context.runBlocking([] {
        std::this_thread::sleep_for(150ms);
        return std::string("slow-done");
    });
    co_return context.text(std::string_view(slow));
}

ruvia::Task<ruvia::HttpResponse> fastHandler(void*, ruvia::Context& context) {
    co_return context.text("fast-done");
}

ruvia::Task<ruvia::HttpResponse> throwingHandler(void*, ruvia::Context& context) {
    const auto value = co_await context.runBlocking([]() -> int { throw std::runtime_error("blocking work failed"); });
    co_return context.text(value == 0 ? std::string_view("zero") : std::string_view("nonzero"));
}

// The non-throwing spelling: an overloaded pool is a status to act on, not an
// error response.
ruvia::Task<ruvia::HttpResponse> tryHandler(void*, ruvia::Context& context) {
    auto result = co_await context.tryRunBlocking([] { return 1; });
    if (result.completed()) {
        co_return context.text("try-completed");
    }
    co_return context.text(std::string_view(ruvia::describeBlockingStatus(result.status())));
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

void registerRoute(ruvia::detail::RouterImpl& impl, std::string_view path, ruvia::Task<ruvia::HttpResponse> (*handler)(void*, ruvia::Context&)) {
    impl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string(path, std::pmr::get_default_resource()), ruvia::detail::RouteHandler(nullptr, handler), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
}

void writeRequest(asio::ip::tcp::socket& socket, std::string_view path, std::error_code& ec) {
    std::string request("GET ");
    request.append(path);
    request.append(" HTTP/1.1\r\nHost: localhost\r\n\r\n");
    asio::write(socket, asio::buffer(request), ec);
}

}  // namespace

int main() {
    int rc = 0;
    auto fail = [&](int code, const char* message) {
        std::fputs(message, stderr);
        std::fputc('\n', stderr);
        rc = code;
    };

    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    registerRoute(impl, "/hold", &holdHandler);
    registerRoute(impl, "/slow", &slowHandler);
    registerRoute(impl, "/fast", &fastHandler);
    registerRoute(impl, "/boom", &throwingHandler);
    registerRoute(impl, "/try", &tryHandler);
    impl.finalize();

    // A worker whose handler is waiting on blocking work keeps serving its
    // other connections.
    {
        ruvia::BlockingPool pool(ruvia::BlockingPoolOptions{.threadCount = 2});
        ruvia::detail::HttpServerOptions options;
        options.blockingPool = &pool;
        ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), impl.routeTable(), {}, options);
        server.start();
        const auto endpoint = server.localEndpoint();

        asio::io_context ctx;
        std::error_code ec;
        asio::ip::tcp::socket slowSocket(ctx);
        asio::ip::tcp::socket fastSocket(ctx);
        slowSocket.connect(endpoint, ec);
        fastSocket.connect(endpoint, ec);
        asio::streambuf slowBuffer;
        asio::streambuf fastBuffer;

        const auto started = std::chrono::steady_clock::now();
        writeRequest(slowSocket, "/slow", ec);
        writeRequest(fastSocket, "/fast", ec);
        const auto fastResponse = readResponse(fastSocket, fastBuffer, ec);
        const auto fastElapsed = std::chrono::steady_clock::now() - started;
        const auto slowResponse = readResponse(slowSocket, slowBuffer, ec);

        if (fastResponse.find("fast-done") == std::string::npos) {
            fail(1, "the second connection did not get its response");
        }
        if (rc == 0 && fastElapsed > 100ms) {
            fail(2, "blocking work in one handler stalled the whole worker");
        }
        if (rc == 0 && slowResponse.find("slow-done") == std::string::npos) {
            fail(3, "the offloaded result did not come back to its handler");
        }

        // What the callable threw is the handler's exception, so the app's
        // error path answers it -- 500 by default, not a pool-level status.
        if (rc == 0) {
            writeRequest(fastSocket, "/boom", ec);
            if (!readResponse(fastSocket, fastBuffer, ec).starts_with("HTTP/1.1 500")) {
                fail(4, "an exception from blocking work was not raised in the handler");
            }
        }
        if (rc == 0) {
            writeRequest(fastSocket, "/try", ec);
            if (readResponse(fastSocket, fastBuffer, ec).find("try-completed") == std::string::npos) {
                fail(5, "tryRunBlocking did not report a completed operation");
            }
        }

        slowSocket.close(ec);
        fastSocket.close(ec);
        server.stop();
        server.join();
    }

    // A saturated pool is capacity, not a bug: 503 for runBlocking(), a status
    // for tryRunBlocking().
    if (rc == 0) {
        ruvia::BlockingPool pool(ruvia::BlockingPoolOptions{.threadCount = 1, .queueCapacity = 1});
        ruvia::detail::HttpServerOptions options;
        options.blockingPool = &pool;
        ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), impl.routeTable(), {}, options);
        server.start();
        const auto endpoint = server.localEndpoint();

        asio::io_context ctx;
        std::error_code ec;
        asio::ip::tcp::socket runningSocket(ctx);
        asio::ip::tcp::socket queuedSocket(ctx);
        asio::ip::tcp::socket refusedSocket(ctx);
        runningSocket.connect(endpoint, ec);
        queuedSocket.connect(endpoint, ec);
        refusedSocket.connect(endpoint, ec);
        asio::streambuf runningBuffer;
        asio::streambuf queuedBuffer;
        asio::streambuf refusedBuffer;

        // Saturation is one running task plus one queued, and the queue holds
        // exactly one: until the pool thread has taken the first task, the one
        // slot is still occupied by it and a second request is refused rather
        // than queued. Waiting for the first to be running is what makes the
        // second one's fate the behaviour under test instead of a race.
        writeRequest(runningSocket, "/hold", ec);
        if (!waitForPool(pool, [](const ruvia::BlockingPoolStats& stats) { return stats.running == 1 && stats.queued == 0; })) {
            fail(6, "the pool never started the first offload");
        }
        if (rc == 0) {
            writeRequest(queuedSocket, "/hold", ec);
            if (!waitForPool(pool, [](const ruvia::BlockingPoolStats& stats) { return stats.running == 1 && stats.queued == 1; })) {
                fail(6, "the pool never reached its configured saturation point");
            }
        }

        if (rc == 0) {
            writeRequest(refusedSocket, "/slow", ec);
            if (!readResponse(refusedSocket, refusedBuffer, ec).starts_with("HTTP/1.1 503")) {
                fail(7, "a saturated pool did not answer with 503");
            }
        }
        if (rc == 0) {
            writeRequest(refusedSocket, "/try", ec);
            const auto response = readResponse(refusedSocket, refusedBuffer, ec);
            if (!response.starts_with("HTTP/1.1 200") || response.find("queue is full") == std::string::npos) {
                fail(8, "tryRunBlocking did not report the saturated pool");
            }
        }

        g_hold.release(2);
        if (rc == 0 && readResponse(runningSocket, runningBuffer, ec).find("held") == std::string::npos) {
            fail(9, "the running offload did not complete after release");
        }
        if (rc == 0 && readResponse(queuedSocket, queuedBuffer, ec).find("held") == std::string::npos) {
            fail(10, "the queued offload did not complete after release");
        }

        runningSocket.close(ec);
        queuedSocket.close(ec);
        refusedSocket.close(ec);
        server.stop();
        server.join();
    }

    // No pool configured: a loud failure, never a blocked worker.
    if (rc == 0) {
        ruvia::detail::HttpServerOptions options;
        ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), impl.routeTable(), {}, options);
        server.start();
        const auto endpoint = server.localEndpoint();

        asio::io_context ctx;
        std::error_code ec;
        asio::ip::tcp::socket socket(ctx);
        socket.connect(endpoint, ec);
        asio::streambuf buffer;
        writeRequest(socket, "/fast", ec);
        if (readResponse(socket, buffer, ec).find("fast-done") == std::string::npos) {
            fail(11, "a server without a pool could not serve ordinary routes");
        }
        if (rc == 0) {
            writeRequest(socket, "/slow", ec);
            if (!readResponse(socket, buffer, ec).starts_with("HTTP/1.1 500")) {
                fail(12, "an unconfigured pool did not fail the request");
            }
        }
        socket.close(ec);
        server.stop();
        server.join();
    }

    return rc;
}
