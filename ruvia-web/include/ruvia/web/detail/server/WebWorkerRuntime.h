#pragma once

#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/RuntimeLifecycle.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/WorkerSubmissionView.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/WebWorker.h"
#include "ruvia/web/detail/http3/Http3WorkerServer.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"
#include "ruvia/web/detail/server/HttpServerListener.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/HttpServerWorkerCompletion.h"
#include "ruvia/web/detail/server/HttpServerWorkerState.h"
#include "ruvia/web/detail/server/NativeAcceptedSocketTicket.h"
#include "ruvia/web/detail/server/session/HttpConnectionState.h"

namespace ruvia::detail {

using TcpSocket = asio::ip::tcp::socket;

class ContextServices;
class AcceptedConnectionLease;
class RouteTable;
class ValidatedHttpServerConfiguration;
class WebWorkerDispatch;

class WebWorkerRuntime final {
public:
    WebWorkerRuntime(std::span<const HttpServerListenerDefinition> listeners,
        const RouteTable& routes, WorkerCapabilityDefinitions capabilities = {},
        HttpServerOptions options = {});
    WebWorkerRuntime(HttpServerListenerDefinition listener, const RouteTable& routes,
        WorkerCapabilityDefinitions capabilities = {}, HttpServerOptions options = {});
    WebWorkerRuntime(asio::ip::tcp::endpoint endpoint, const RouteTable& routes,
        WorkerCapabilityDefinitions capabilities = {}, HttpServerOptions options = {});
    WebWorkerRuntime(const ValidatedHttpServerConfiguration& configuration,
        const RouteTable& routes, WorkerCapabilityDefinitions capabilities);
    ~WebWorkerRuntime();

    WebWorkerRuntime(const WebWorkerRuntime&) = delete;
    WebWorkerRuntime& operator=(const WebWorkerRuntime&) = delete;

    // Single-worker convenience. App uses the explicit phases below so no
    // listener accepts before every worker is ready.
    void start();
    void prepare();
    void launch();
    void waitUntilReady();
    void requestServe();
    [[nodiscard]] bool waitUntilServing();
    void stop() noexcept;
    // App shutdown closes worker admission before joining the server network
    // runtime, but defers worker-owned teardown until it can no longer submit
    // callbacks. HTTP/3 also completes its bidirectional worker/network finalization ACK.
    void stopAdmission() noexcept;
    void finalizeAfterNetworkQuiesced() noexcept;
    // Lifecycle owners join from outside the server worker. Reject self-join
    // before touching std::thread so behavior is deterministic across platforms.
    void join();
    [[nodiscard]] asio::ip::tcp::endpoint localEndpoint(std::size_t listenerIndex = 0) const;
    [[nodiscard]] asio::io_context::executor_type workerExecutor() noexcept {
        return ioContext_.get_executor();
    }
    // Lock-free admission snapshot for the server network dispatcher. A true
    // result is advisory; the worker rechecks capacity when the socket is delivered.
    [[nodiscard]] bool availableForNetworkDispatch() const noexcept;
    // Takes ownership immediately. Returns true only when delivery was committed;
    // otherwise closes the socket and returns false. Never throws.
    // Called on the worker after server network mailbox delivery. Rechecks worker state
    // and capacity, assigns before any I/O, and consumes the ticket on every path.
    void acceptTransferredConnection(NativeAcceptedSocketTicket&& ticket) noexcept;
    // Safe from any thread, at any point in the lifecycle.
    [[nodiscard]] HttpServerStats stats() const noexcept;
    [[nodiscard]] WorkerSubmissionView networkSubmission() const& noexcept {
        return workerRuntime_.submission();
    }
    WorkerSubmissionView networkSubmission() const&& = delete;
    [[nodiscard]] const WorkerHandle& worker() const& noexcept {
        return workerRuntime_.handle();
    }
    WorkerHandle worker() const&& = delete;
    [[nodiscard]] WebWorkerHandle webWorker() const;
    [[nodiscard]] Http3WorkerServer* http3Server() noexcept {
        return http3Server_.get();
    }

private:
    struct ValidatedConfigurationTag final {};
    enum class ConnectionOwnershipMode { kOwnListeners,
        kTransferredSessions };
    using DocumentRootPtr = std::unique_ptr<StaticRoot, PmrObjectDeleter<StaticRoot>>;
    using ListenerPtr =
        std::unique_ptr<HttpServerSessionConfig, PmrObjectDeleter<HttpServerSessionConfig>>;
    using AcceptorPtr = std::unique_ptr<HttpServerAcceptor, PmrObjectDeleter<HttpServerAcceptor>>;

    WebWorkerRuntime(const ValidatedHttpServerConfiguration& configuration,
        const RouteTable& routes, WorkerCapabilityDefinitions capabilities,
        ConnectionOwnershipMode connectionOwnershipMode);
    WebWorkerRuntime(ValidatedConfigurationTag,
        std::span<const HttpServerListenerDefinition> listeners, const RouteTable& routes,
        WorkerCapabilityDefinitions capabilities, HttpServerOptions validatedOptions,
        ConnectionOwnershipMode connectionOwnershipMode);

    void configureAcceptor(HttpServerAcceptor& acceptor);
    void configureTlsContext(HttpServerSessionConfig& session);
    void stopAdmissionOnContext() noexcept;
    void stopOnContext() noexcept;
    void failWorker(const std::exception_ptr& failure) noexcept;
    void runIoContext() noexcept;
    Task<void> runWorker();
    Task<void> staticRootRefreshLoop();
    Task<void> superviseListener(
        std::size_t listenerIndex, HttpServerAcceptor& acceptor);
    Task<void> acceptLoop(std::size_t listenerIndex, HttpServerAcceptor& acceptor);
    void acceptSocketOnContext(std::size_t listenerIndex, TcpSocket socket);
    Task<void> handleSession(HttpServerSessionConfig& listener, AcceptedConnectionLease connection);
    template <typename Stream>
    Task<void> handleStreamSession(HttpServerSessionConfig& listener, Stream& stream,
        asio::ip::tcp::socket& socket, ContextServices services);
    template <typename Stream>
    Task<void> handleHttp2Session(Stream& stream, asio::ip::tcp::socket& socket,
        ContextServices services, std::string_view initialBytes = {});
    asio::io_context ioContext_;
    ruvia::WorkerRuntimeContext workerRuntime_;
    // Keeps the owner loop alive until phase-two finalization after the server
    // network runtime has joined.
    asio::executor_work_guard<asio::io_context::executor_type> finalizeGuard_;
    WorkerSignal serveSignal_;
    WorkerSignal finalizeSignal_;
    StopSource stopSource_;
    StopToken stopToken_{stopSource_.token()};
    const RouteTable& routes_;
    WorkerMemory memory_;
    std::pmr::vector<ListenerPtr> listeners_;
    std::pmr::vector<AcceptorPtr> acceptors_;
    TaskScope backgroundTasks_;
    DocumentRootPtr ownedDocumentRoot_;
    std::pmr::vector<DocumentRootPtr> retiredDocumentRoots_;
    HttpServerOptions options_;
    ConnectionOwnershipMode connectionOwnershipMode_{ConnectionOwnershipMode::kOwnListeners};
    ruvia::ConnectionScanner connectionScanner_;
    WorkerCapabilities capabilities_;
    std::unique_ptr<Http3WorkerServer, PmrObjectDeleter<Http3WorkerServer>> http3Server_;
    std::shared_ptr<WebWorkerDispatch> webWorkerDispatch_;
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
    std::mutex threadStateMutex_;
    bool threadLaunched_{false};
    bool prepared_{false};
    bool serveRequested_{false};
    std::atomic<bool> networkServing_{false};

    HttpServerWorkerCompletion workerCompletion_;
};

}  // namespace ruvia::detail
