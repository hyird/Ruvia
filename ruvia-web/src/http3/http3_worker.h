#pragma once

#include <atomic>
#include <memory>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_runtime_context.h"

#include "http3/http3_worker_runtime.h"
#include "http3/http3_worker_server.h"

namespace ruvia::detail {

// Owns the complete worker-local HTTP/3 component. Core owns the execution loop;
// Acceptor owns the borrowed datagram channel until this component's final ACK.
class http3_worker final {
public:
    http3_worker(worker_runtime_context& runtime, worker_memory& memory,
        const route_table& routes_value, worker_capabilities& capabilities,
        connection_scanner& scanner, const http_server_options& options,
        const stop_token& stop_token_value, const http_server_listener_definition::tls_type& tls,
        http3_listen_config config, std::atomic<std::size_t>& active_connections,
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
    [[nodiscard]] task<void> join();

private:
    worker_runtime_context& runtime_;
    worker_memory& memory_;
    const http_server_options& options_;
    const stop_token& stop_token_;
    const http_server_listener_definition::tls_type& tls_;
    http3_listen_config config_;
    http3_worker_runtime::failure_notification failure_;
    http3_worker_server server_;
    std::unique_ptr<http3_worker_runtime, pmr_object_deleter<http3_worker_runtime>> transport_;
    task_scope tasks_;
    asio::ip::udp::endpoint endpoint_;
    quic_cid_partition partition_;
    std::atomic<http3_datagram_channel*> staged_channel_{};
    std::atomic<bool> abandoned_{};
    bool started_{};
};

}  // namespace ruvia::detail
