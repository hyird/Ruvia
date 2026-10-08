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

#include "integration/WorkerCapabilities.h"
#include "server/HttpConnectionState.h"
#include "server/HttpServerListener.h"
#include "server/HttpServerOptions.h"
#include "server/HttpServerWorkerState.h"
#include "server/NativeAcceptedSocketTicket.h"
#include "server/inbound_buffer_resource.h"
#include "server/runtime_completion.h"
#include "server/static_root_runtime.h"
#include "server/worker_connections.h"

namespace ruvia::detail {

class ContextServices;
class AcceptedConnectionLease;
class RouteTable;
class ValidatedHttpServerConfiguration;
class WebWorkerDispatch;
class http3_datagram_channel;
class http3_worker;
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
    // Called on the worker after bounded acceptor queue delivery. Rechecks worker state
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

    WebWorkerRuntime(const ValidatedHttpServerConfiguration& configuration,
        const RouteTable& routes, WorkerCapabilityDefinitions capabilities,
        bool own_acceptor);
    WebWorkerRuntime(ValidatedConfigurationTag,
        std::span<const HttpServerListenerDefinition> listeners, const RouteTable& routes,
        WorkerCapabilityDefinitions capabilities, HttpServerOptions validatedOptions,
        bool own_acceptor);

    void stopAdmissionOnContext() noexcept;
    void stopOnContext() noexcept;
    void failWorker(const std::exception_ptr& failure) noexcept;
    void stop_http3() noexcept;
    Task<void> runWorker();
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
    std::unique_ptr<acceptor, PmrObjectDeleter<acceptor>> owned_acceptor_;
    TaskScope backgroundTasks_;
    HttpServerWorkerState workerState_{HttpServerWorkerState::kFresh};
    HttpServerOptions options_;
    static_root_runtime static_roots_;
    WorkerCapabilities capabilities_;
    worker_connections connections_;
    std::unique_ptr<http3_worker, PmrObjectDeleter<http3_worker>> http3_;
    std::shared_ptr<WebWorkerDispatch> webWorkerDispatch_;
    // Atomic because stats() reads them from the caller's thread while this
    // worker updates them. Relaxed: they are counters, and publish nothing.
    std::atomic<std::size_t> workerFailures_{0};

    // Core owns the external lifecycle. Request coroutines only observe this
    // worker-local business state.
    bool prepared_{false};
    bool serveRequested_{false};

    runtime_completion workerCompletion_;
};

}  // namespace ruvia::detail
