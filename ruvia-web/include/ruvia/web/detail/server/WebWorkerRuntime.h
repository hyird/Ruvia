#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <exception>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "ruvia/core/Task.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/RuntimeLifecycle.h"
#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/WebWorker.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"
#include "ruvia/web/detail/integration/WorkerDataState.h"
#include "ruvia/web/detail/integration/WorkerState.h"
#include "ruvia/web/detail/ratelimit/RateLimiter.h"
#include "ruvia/web/detail/server/HttpServerListener.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/HttpServerWorkerCompletion.h"
#include "ruvia/web/detail/server/HttpServerWorkerState.h"
#include "ruvia/web/detail/server/session/HttpConnectionState.h"

namespace ruvia::detail {

using TcpSocket = asio::ip::tcp::socket;

class ContextServices;
class AcceptedConnectionLease;
class RouteTable;
class WorkerDispatcher;
class WebWorkerDispatch;
struct DbDefinition;
struct HttpClientDefinition;
struct RedisDefinition;
class WorkerStateDefinition;

struct WebWorkerRuntimeResources final {
    std::span<const DbDefinition> databases;
    std::span<const RedisDefinition> redis;
    std::span<const WorkerStateDefinition> workerStates;
    std::span<const HttpClientDefinition> httpClients;
};

class WebWorkerRuntime final {
public:
    WebWorkerRuntime(std::span<const HttpServerListenerDefinition> listeners, const RouteTable& routes, WebWorkerRuntimeResources resources = {}, HttpServerOptions options = {});
    WebWorkerRuntime(HttpServerListenerDefinition listener, const RouteTable& routes, WebWorkerRuntimeResources resources = {}, HttpServerOptions options = {});
    WebWorkerRuntime(asio::ip::tcp::endpoint endpoint, const RouteTable& routes, WebWorkerRuntimeResources resources = {}, HttpServerOptions options = {});
    ~WebWorkerRuntime();

    WebWorkerRuntime(const WebWorkerRuntime&) = delete;
    WebWorkerRuntime& operator=(const WebWorkerRuntime&) = delete;

    void start();
    void stop();
    // Lifecycle owners join from outside the server worker. Reject self-join
    // before touching std::thread so behavior is deterministic across platforms.
    void join();
    [[nodiscard]] asio::ip::tcp::endpoint localEndpoint(ListenerId listener) const;
    // Safe from any thread, at any point in the lifecycle.
    [[nodiscard]] HttpServerStats stats() const noexcept;
    [[nodiscard]] const WorkerHandle& worker() const& noexcept {
        return workerHandle_;
    }
    WorkerHandle worker() const&& = delete;
    [[nodiscard]] WebWorkerHandle webWorker() const;

private:
    struct ValidatedOptionsTag final {};
    using DocumentRootPtr = std::unique_ptr<StaticRoot, PmrObjectDeleter<StaticRoot>>;
    using ListenerPtr = std::unique_ptr<HttpServerListener, PmrObjectDeleter<HttpServerListener>>;

    WebWorkerRuntime(ValidatedOptionsTag, std::span<const HttpServerListenerDefinition> listeners, const RouteTable& routes, WebWorkerRuntimeResources resources, HttpServerOptions validatedOptions);

    void configureAcceptor(HttpServerListener& listener);
    void configureTlsContext(HttpServerListener& listener);
    void stopOnContext() noexcept;
    void failWorker(std::exception_ptr failure) noexcept;
    void runIoContext() noexcept;
    Task<void> runWorker();
    Task<void> staticRootRefreshLoop();
    Task<void> superviseListener(HttpServerListener& listener);
    Task<void> acceptLoop(HttpServerListener& listener);
    Task<void> handleSession(HttpServerListener& listener, AcceptedConnectionLease connection);
    template <typename Stream>
    Task<void> handleStreamSession(HttpServerListener& listener, Stream& stream, asio::ip::tcp::socket& socket, ContextServices services);
    template <typename Stream>
    Task<void> handleHttp2Session(Stream& stream, asio::ip::tcp::socket& socket, ContextServices services, std::string_view initialBytes = {});
    asio::io_context ioContext_;
    std::shared_ptr<WorkerDispatcher> workerDispatcher_;
    WorkerHandle workerHandle_;
    StopSource stopSource_;
    StopToken stopToken_{stopSource_.token()};
    const RouteTable& routes_;
    WorkerMemory memory_;
    std::pmr::vector<ListenerPtr> listeners_;
    TaskScope backgroundTasks_;
    DocumentRootPtr ownedDocumentRoot_;
    std::pmr::vector<DocumentRootPtr> retiredDocumentRoots_;
    HttpServerOptions options_;
    ConnectionScanner connectionScanner_;
    WorkerDataState workerData_;
    HttpClientRegistry httpClients_;
    WorkerStateRegistry workerStates_;
    std::shared_ptr<WebWorkerDispatch> webWorkerDispatch_;
    RateLimiter rateLimiter_;
    ConnectionWorkSetPool workSetPool_;
    // Atomic because stats() reads them from the caller's thread while this
    // worker updates them. Relaxed: they are counters, and publish nothing.
    std::atomic<std::size_t> activeConnectionCount_{0};
    std::atomic<std::size_t> connectionsRefused_{0};
    std::atomic<std::size_t> connectionFailures_{0};
    std::atomic<std::size_t> acceptFailures_{0};
    std::atomic<std::size_t> workerFailures_{0};
    std::atomic<std::size_t> documentRootRefreshFailures_{0};

    // lifecycle_ is touched by external start/stop callers. Request coroutines
    // observe workerState_, which is mutated only on this io_context.
    RuntimeLifecycle lifecycle_;
    HttpServerWorkerState workerState_{HttpServerWorkerState::kFresh};
    std::thread workerThread_;

    HttpServerWorkerCompletion workerCompletion_;
};

}  // namespace ruvia::detail
