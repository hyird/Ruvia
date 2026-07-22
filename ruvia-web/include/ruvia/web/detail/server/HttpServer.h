#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/RuntimeLifecycle.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/web/WebWorker.h"
#include "ruvia/web/detail/integration/DataAccessState.h"
#include "ruvia/web/detail/server/session/HttpConnectionState.h"
#include "ruvia/web/detail/ratelimit/RateLimiter.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/HttpServerWorkerState.h"
#include "ruvia/web/detail/server/HttpServerWorkerCompletion.h"
#include "ruvia/web/detail/integration/WorkerState.h"
namespace ruvia::detail {

class ContextServices;
class AcceptedConnectionLease;
class RouteTable;
class WorkerDispatcher;
class WebWorkerDispatch;

using SniContextStore = std::pmr::vector<asio::ssl::context>;
using SniContextLookup = std::pmr::vector<std::pair<std::pmr::string, asio::ssl::context*>>;

class HttpServer final {
public:
    HttpServer(
        asio::ip::tcp::endpoint endpoint,
        const RouteTable& routes,
        std::span<const DbDefinition> databases = {},
        HttpServerOptions options = {});
    HttpServer(
        asio::ip::tcp::endpoint endpoint,
        const RouteTable& routes,
        std::span<const DbDefinition> databases,
        std::span<const RedisDefinition> redis,
        HttpServerOptions options = {});
    HttpServer(
        asio::ip::tcp::endpoint endpoint,
        const RouteTable& routes,
        std::span<const DbDefinition> databases,
        std::span<const RedisDefinition> redis,
        std::span<const WorkerStateDefinition> workerStates,
        HttpServerOptions options = {});
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void start();
    void stop();
    // Lifecycle owners join from outside the server worker. Reject self-join
    // before touching std::thread so behavior is deterministic across platforms.
    void join();
    [[nodiscard]] asio::ip::tcp::endpoint localEndpoint() const;
    [[nodiscard]] const WorkerHandle& worker() const & noexcept {
        return workerHandle_;
    }
    WorkerHandle worker() const && = delete;
    [[nodiscard]] WebWorkerHandle webWorker() const;
private:
    struct ValidatedOptionsTag final {};

    HttpServer(
        ValidatedOptionsTag,
        asio::ip::tcp::endpoint endpoint,
        const RouteTable& routes,
        std::span<const DbDefinition> databases,
        std::span<const RedisDefinition> redis,
        std::span<const WorkerStateDefinition> workerStates,
        HttpServerOptions validatedOptions);

    void configureAcceptor();
    void configureTlsContext();
    void stopOnContext() noexcept;
    void failWorker(std::exception_ptr failure) noexcept;
    void runIoContext() noexcept;
    Task<void> runWorker();
    Task<void> acceptLoop();
    Task<void> handleSession(AcceptedConnectionLease connection);
    template <typename Stream>
    Task<void> handleStreamSession(
        Stream& stream,
        asio::ip::tcp::socket& socket,
        ContextServices services);
    template <typename Stream>
    Task<void> handleHttp2Session(
        Stream& stream,
        asio::ip::tcp::socket& socket,
        ContextServices services,
        std::string_view initialBytes = {});
    asio::io_context ioContext_;
    std::shared_ptr<WorkerDispatcher> workerDispatcher_;
    WorkerHandle workerHandle_;
    asio::ip::tcp::acceptor acceptor_;
    std::optional<asio::ssl::context> tlsContext_;
    asio::ip::tcp::endpoint endpoint_;
    const RouteTable& routes_;
    WorkerMemory memory_;
    // Per-host SNI contexts (RFC 6066), owned here so they outlive connections;
    // sniLookup_ maps a lowercased host to its context for the SNI callback.
    SniContextStore sniContexts_;
    SniContextLookup sniLookup_;
    HttpServerOptions options_;
    ConnectionScanner connectionScanner_;
    DataAccessState dataAccess_;
    WorkerStateRegistry workerStates_;
    std::shared_ptr<WebWorkerDispatch> webWorkerDispatch_;
    RateLimiter rateLimiter_;
    ConnectionWorkSetPool workSetPool_;
    std::size_t activeConnectionCount_{0};

    // lifecycle_ is touched by external start/stop callers. Request coroutines
    // observe workerState_, which is mutated only on this io_context.
    RuntimeLifecycle lifecycle_;
    HttpServerWorkerState workerState_{HttpServerWorkerState::kFresh};
    std::thread workerThread_;

    HttpServerWorkerCompletion workerCompletion_;
};

}  // namespace ruvia::detail
