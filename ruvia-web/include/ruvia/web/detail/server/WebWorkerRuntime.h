#pragma once

#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/WorkerSubmissionView.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/http/quic_server.h"
#include "ruvia/web/WebWorker.h"
#include "ruvia/web/detail/http3/http3_worker_server.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"
#include "ruvia/web/detail/server/HttpServerListener.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/HttpServerWorkerCompletion.h"
#include "ruvia/web/detail/server/HttpServerWorkerState.h"
#include "ruvia/web/detail/server/NativeAcceptedSocketTicket.h"
#include "ruvia/web/detail/server/inbound_buffer_resource.h"
#include "ruvia/web/detail/server/session/HttpConnectionState.h"

namespace ruvia::detail {

using TcpSocket = asio::ip::tcp::socket;

class ContextServices;
class AcceptedConnectionLease;
class RouteTable;
class ValidatedHttpServerConfiguration;
class WebWorkerDispatch;
class http3_datagram_channel;
class http3_worker_runtime;
class acceptor;

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
    // Close admission first; the acceptor/worker datagram channel must finish
    // both owners' closure before final worker teardown.
    void stopAdmission() noexcept;
    void finalizeAfterNetworkQuiesced() noexcept;
    // Lifecycle owners join from outside the server worker. Reject self-join
    // before touching std::thread so behavior is deterministic across platforms.
    void join();
    [[nodiscard]] asio::ip::tcp::endpoint localEndpoint(std::size_t listenerIndex = 0) const;
    [[nodiscard]] asio::io_context::executor_type workerExecutor() noexcept {
        return ioContext_.get_executor();
    }
    // Lock-free admission snapshot for the acceptor. A true
    // result is advisory; the worker rechecks capacity when the socket is delivered.
    [[nodiscard]] bool availableForNetworkDispatch() const noexcept;
    // Called on the worker after bounded acceptor mailbox delivery. Rechecks worker state
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
    // Cold staging only, before launch. Protocol objects are constructed on
    // this worker, not by the ingress thread that supplies the datagram channel.
    void stage_quic(http3_datagram_channel& channel, asio::ip::udp::endpoint endpoint,
        ruvia::quic_cid_partition partition);

private:
    struct ValidatedConfigurationTag final {};
    using DocumentRootPtr = std::unique_ptr<StaticRoot, PmrObjectDeleter<StaticRoot>>;
    using ListenerPtr =
        std::unique_ptr<HttpServerSessionConfig, PmrObjectDeleter<HttpServerSessionConfig>>;

    WebWorkerRuntime(const ValidatedHttpServerConfiguration& configuration,
        const RouteTable& routes, WorkerCapabilityDefinitions capabilities,
        bool own_acceptor);
    WebWorkerRuntime(ValidatedConfigurationTag,
        std::span<const HttpServerListenerDefinition> listeners, const RouteTable& routes,
        WorkerCapabilityDefinitions capabilities, HttpServerOptions validatedOptions,
        bool own_acceptor);

    void configureTlsContext(HttpServerSessionConfig& session);
    void stopAdmissionOnContext() noexcept;
    void stopOnContext() noexcept;
    void failWorker(const std::exception_ptr& failure) noexcept;
    void start_http3();
    void stop_http3() noexcept;
    Task<void> runWorker();
    Task<void> staticRootRefreshLoop();
    void acceptSocketOnContext(std::size_t listenerIndex, TcpSocket socket);
    Task<void> handleSession(HttpServerSessionConfig& listener, AcceptedConnectionLease connection);
    template <typename Stream>
    Task<void> handleStreamSession(HttpServerSessionConfig& listener, Stream& stream,
        asio::ip::tcp::socket& socket, ContextServices services);
    template <typename Stream>
    Task<void> handleHttp2Session(Stream& stream, asio::ip::tcp::socket& socket,
        ContextServices services, std::string_view initialBytes = {});
    ruvia::worker_runtime runtime_;
    asio::io_context& ioContext_;
    ruvia::WorkerRuntimeContext& workerRuntime_;
    WorkerSignal serveSignal_;
    WorkerSignal finalizeSignal_;
    StopSource stopSource_;
    StopToken stopToken_{stopSource_.token()};
    const RouteTable& routes_;
    WorkerMemory memory_;
    inbound_buffer_resource inbound_buffers_;
    std::pmr::vector<ListenerPtr> listeners_;
    std::unique_ptr<acceptor, PmrObjectDeleter<acceptor>> owned_acceptor_;
    TaskScope backgroundTasks_;
    DocumentRootPtr ownedDocumentRoot_;
    std::pmr::vector<DocumentRootPtr> retiredDocumentRoots_;
    HttpServerOptions options_;
    ruvia::ConnectionScanner connectionScanner_;
    WorkerCapabilities capabilities_;
    std::unique_ptr<http3_worker_server, PmrObjectDeleter<http3_worker_server>> http3Server_;
    std::unique_ptr<http3_worker_runtime, PmrObjectDeleter<http3_worker_runtime>> http3_transport_;
    std::optional<Http3ListenConfig> http3_config_;
    std::size_t http3_listener_index_{};
    asio::ip::udp::endpoint quic_endpoint_;
    ruvia::quic_cid_partition cid_partition_;
    std::atomic<http3_datagram_channel*> quic_channel_{};
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

    // Core owns the external lifecycle. Request coroutines only observe this
    // worker-local business state.
    HttpServerWorkerState workerState_{HttpServerWorkerState::kFresh};
    bool prepared_{false};
    bool serveRequested_{false};
    std::atomic<bool> networkServing_{false};

    HttpServerWorkerCompletion workerCompletion_;
};

}  // namespace ruvia::detail
