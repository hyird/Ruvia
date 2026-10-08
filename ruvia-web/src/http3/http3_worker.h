#pragma once

#include <atomic>
#include <memory>

#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerRuntimeContext.h"

#include "http3/http3_worker_runtime.h"
#include "http3/http3_worker_server.h"

namespace ruvia::detail {

// Owns the complete worker-local HTTP/3 component. Core owns the execution loop;
// Acceptor owns the borrowed datagram channel until this component's final ACK.
class http3_worker final {
public:
    http3_worker(WorkerRuntimeContext& runtime, WorkerMemory& memory,
        const RouteTable& routes, WorkerCapabilities& capabilities,
        ConnectionScanner& scanner, const HttpServerOptions& options,
        const StopToken& stop_token, const HttpServerListenerDefinition::Tls& tls,
        Http3ListenConfig config, std::atomic<std::size_t>& active_connections,
        std::atomic<std::size_t>& refused_connections,
        http3_worker_runtime::failure_notification failure);
    ~http3_worker();
    http3_worker(const http3_worker&) = delete;
    http3_worker& operator=(const http3_worker&) = delete;

    // Cold ingress staging can race cancellation before core launches its loop.
    void stage(http3_datagram_channel&, asio::ip::udp::endpoint, quic_cid_partition);
    void abandon_before_launch() noexcept;
    // Owner-loop operations. start rolls back admission on failure; join retires
    // every launched task and destroys protocol state on its owning thread.
    void start();
    void stop() noexcept;
    [[nodiscard]] Task<void> join();

private:
    WorkerRuntimeContext& runtime_;
    WorkerMemory& memory_;
    const HttpServerOptions& options_;
    const StopToken& stop_token_;
    const HttpServerListenerDefinition::Tls& tls_;
    Http3ListenConfig config_;
    http3_worker_runtime::failure_notification failure_;
    http3_worker_server server_;
    std::unique_ptr<http3_worker_runtime, PmrObjectDeleter<http3_worker_runtime>> transport_;
    TaskScope tasks_;
    asio::ip::udp::endpoint endpoint_;
    quic_cid_partition partition_;
    std::atomic<http3_datagram_channel*> staged_channel_{};
    std::atomic<bool> abandoned_{};
    bool started_{};
};

}  // namespace ruvia::detail
