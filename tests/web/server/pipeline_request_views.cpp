// Every view in `parsed.request` -- method, target, and all header values --
// borrows the connection read buffer. A pipelined request must therefore not be
// shifted to the front of that buffer until the request that owns those views is
// done with them. The buffered-body route used to restore the pipeline the
// moment the handler returned, which is before the response is built and before
// the access log is recorded, so request 1 was logged with request 2's path --
// an attacker could pipeline a request to make /admin/delete vanish from the
// audit trail.
//
// Two requests are written as one segment so the second is guaranteed to be
// sitting in the read buffer while the first is still being completed.

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/ServerConfig.h"
#include "ruvia/web/detail/util/CallableRef.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

namespace {

constexpr auto kResponseBound = std::chrono::seconds(10);

class AccessLogListener final {
public:
    void operator()(const ruvia::AccessLogRecord& record) noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        paths_.emplace_back(record.path());
    }

    [[nodiscard]] std::vector<std::string> paths() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return paths_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> paths_;
};

template <typename Handler>
void registerRoute(ruvia::detail::RouterImpl& router, ruvia::HttpKnownMethod method, std::string_view path, Handler& handler) {
    router.registerRoute(method, std::pmr::string(path, std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(handler), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
}

// Returns the paths the server logged for one pipelined burst, or an empty
// vector if fewer than `expected` records arrived before the bound.
std::vector<std::string> logPipelinedBurst(const asio::ip::tcp::endpoint& endpoint, const AccessLogListener& listener, std::string_view pipelined, std::size_t expected) {
    asio::io_context clientContext;
    asio::ip::tcp::socket client(clientContext);
    client.connect(endpoint);
    // One segment, two requests: the second sits in the read buffer for the
    // whole of the first request's completion.
    asio::write(client, asio::buffer(pipelined));

    const auto deadline = std::chrono::steady_clock::now() + kResponseBound;
    std::vector<std::string> paths;
    for (;;) {
        paths = listener.paths();
        if (paths.size() >= expected || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::error_code ignored;
    client.close(ignored);
    return paths;
}

}  // namespace

int main() {
    ruvia::detail::Router router;
    auto& routerImpl = ruvia::detail::RouterImpl::from(router);
    // The route table borrows the handlers, so they must outlive the server.
    auto firstHandler = [](ruvia::Context& c) -> ruvia::Task<ruvia::HttpResponse> { co_return c.text("one"); };
    auto secondHandler = [](ruvia::Context& c) -> ruvia::Task<ruvia::HttpResponse> { co_return c.text("two"); };
    // Consuming the body is what lets the connection stay alive to serve the
    // pipelined successor, and it drives the known-length remainder path.
    auto uploadHandler = [](ruvia::Context& c) -> ruvia::Task<ruvia::HttpResponse> {
        const auto body = co_await c.req().text();
        co_return c.text(body);
    };
    registerRoute(routerImpl, ruvia::HttpKnownMethod::kGet, "/first", firstHandler);
    registerRoute(routerImpl, ruvia::HttpKnownMethod::kGet, "/second", secondHandler);
    registerRoute(routerImpl, ruvia::HttpKnownMethod::kPost, "/upload", uploadHandler);
    routerImpl.finalize();

    AccessLogListener listener;
    ruvia::detail::HttpServerOptions options;
    options.accessLog.callback = ruvia::detail::CallbackAccess::bind<void(const ruvia::AccessLogRecord&) noexcept>(listener);

    ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routerImpl.routeTable(), {}, std::move(options));
    server.start();

    // A bodyless request keeps the whole pipelined successor in the read buffer.
    const auto bodyless = logPipelinedBurst(server.localEndpoint(ruvia::ListenerId{1}), listener,
        "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n",
        2);

    // A known-length body puts the successor behind consumed body bytes, which
    // is a separate remainder path in the body reader.
    const auto afterBody = logPipelinedBurst(server.localEndpoint(ruvia::ListenerId{1}), listener,
        "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello"
        "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n",
        4);

    server.stop();
    server.join();

    // Both bursts are checked before returning: each exercises a different
    // pipeline-remainder path, so a regression in one must not be masked by the
    // other failing first.
    int failures = 0;
    const auto check = [&failures](const char* label, const std::vector<std::string>& got, std::size_t at, std::string_view wantFirst, std::string_view wantSecond) {
        if (got.size() < at + 2) {
            std::fprintf(stderr, "%s: expected at least %zu access log records, got %zu\n", label, at + 2, got.size());
            ++failures;
            return;
        }
        if (got[at] == wantFirst && got[at + 1] == wantSecond) {
            return;
        }
        std::fprintf(stderr,
            "%s: pipelined request corrupted the logged path: got {\"%s\", "
            "\"%s\"}, want {\"%.*s\", \"%.*s\"}\n",
            label, got[at].c_str(), got[at + 1].c_str(), static_cast<int>(wantFirst.size()), wantFirst.data(), static_cast<int>(wantSecond.size()), wantSecond.data());
        ++failures;
    };

    check("bodyless", bodyless, 0, "/first", "/second");
    check("after known-length body", afterBody, 2, "/upload", "/second");
    return failures == 0 ? 0 : 1;
}
