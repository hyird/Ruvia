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

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_submission_view.h"
#include "ruvia/http/quic_server.h"
#include "ruvia/web/web_worker.h"

#include "integration/worker_capabilities.h"
#include "server/http_connection_state.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"
#include "server/http_server_worker_state.h"
#include "server/inbound_buffer_resource.h"
#include "server/native_accepted_socket_ticket.h"
#include "server/runtime_completion.h"
#include "server/static_root_runtime.h"
#include "server/worker_connections.h"

namespace ruvia::detail {

class context_services;
class accepted_connection_lease;
class route_table;
class validated_http_server_configuration;
class web_worker_dispatch;
class http3_datagram_channel;
class http3_worker;
class acceptor;

class web_worker_runtime final {
public:
    web_worker_runtime(std::span<const http_server_listener_definition> listeners,
        const route_table& routes_value, worker_capability_definitions capabilities = {},
        http_server_options options = {});
    web_worker_runtime(http_server_listener_definition listener_value, const route_table& routes_value,
        worker_capability_definitions capabilities = {}, http_server_options options = {});
    web_worker_runtime(asio::ip::tcp::endpoint endpoint, const route_table& routes_value,
        worker_capability_definitions capabilities = {}, http_server_options options = {});
    web_worker_runtime(const validated_http_server_configuration& configuration,
        const route_table& routes_value, worker_capability_definitions capabilities);
    ~web_worker_runtime();

    web_worker_runtime(const web_worker_runtime&) = delete;
    web_worker_runtime& operator=(const web_worker_runtime&) = delete;

    // Single-worker convenience. application uses the explicit phases below so no
    // listener accepts before every worker is ready.
    void start();
    void prepare();
    void launch();
    void wait_until_ready();
    void request_serve();
    [[nodiscard]] bool wait_until_serving();
    void stop() noexcept;
    // Close admission first; the acceptor/worker datagram channel must finish
    // both owners' closure before final worker teardown.
    void stop_admission() noexcept;
    void finalize_after_network_quiesced() noexcept;
    // Lifecycle owners join from outside the server worker. Reject self-join
    // before touching std::thread so behavior is deterministic across platforms.
    void join();
    [[nodiscard]] asio::ip::tcp::endpoint local_endpoint(std::size_t listener_index = 0) const;
    [[nodiscard]] asio::io_context::executor_type worker_executor() noexcept {
        return io_context_.get_executor();
    }
    // Lock-free admission snapshot for the acceptor. A true
    // result is advisory; the worker rechecks capacity when the socket is delivered.
    [[nodiscard]] bool available_for_network_dispatch() const noexcept;
    // Called on the worker after bounded acceptor queue delivery. Rechecks worker state
    // and capacity, assigns before any I/O, and consumes the ticket on every path.
    void accept_transferred_connection(native_accepted_socket_ticket&& ticket) noexcept;
    // Safe from any thread, at any point in the lifecycle.
    [[nodiscard]] http_server_stats stats() const noexcept;
    [[nodiscard]] worker_submission_view network_submission() const& noexcept {
        return worker_runtime_.submission();
    }
    worker_submission_view network_submission() const&& = delete;
    [[nodiscard]] const worker_handle& worker() const& noexcept {
        return worker_runtime_.handle();
    }
    worker_handle worker() const&& = delete;
    [[nodiscard]] web_worker_handle web_worker() const;
    // Cold staging only, before launch. Protocol objects are constructed on
    // this worker, not by the ingress thread that supplies the datagram channel.
    void stage_quic(http3_datagram_channel& channel, asio::ip::udp::endpoint endpoint,
        ruvia::quic_cid_partition partition);

private:
    struct validated_configuration_tag_type final {};

    web_worker_runtime(const validated_http_server_configuration& configuration,
        const route_table& routes_value, worker_capability_definitions capabilities,
        bool own_acceptor);
    web_worker_runtime(validated_configuration_tag_type,
        std::span<const http_server_listener_definition> listeners, const route_table& routes_value,
        worker_capability_definitions capabilities, http_server_options validated_options,
        bool own_acceptor);

    void stop_admission_on_context() noexcept;
    void stop_on_context() noexcept;
    void fail_worker(const std::exception_ptr& failure) noexcept;
    void stop_http3() noexcept;
    task<void> run_worker();
    ruvia::worker_runtime runtime_;
    asio::io_context& io_context_;
    ruvia::worker_runtime_context& worker_runtime_;
    worker_signal serve_signal_;
    worker_signal finalize_signal_;
    stop_source stop_source_;
    stop_token stop_token_{stop_source_.token()};
    const route_table& routes_;
    worker_memory memory_;
    inbound_buffer_resource inbound_buffers_;
    std::unique_ptr<acceptor, pmr_object_deleter<acceptor>> owned_acceptor_;
    task_scope background_tasks_;
    http_server_worker_state worker_state_{http_server_worker_state::fresh};
    http_server_options options_;
    static_root_runtime static_roots_;
    worker_capabilities capabilities_;
    worker_connections connections_;
    std::unique_ptr<http3_worker, pmr_object_deleter<http3_worker>> http3_;
    std::shared_ptr<web_worker_dispatch> web_worker_dispatch_;
    // Atomic because stats() reads them from the caller's thread while this
    // worker updates them. Relaxed: they are counters, and publish nothing.
    std::atomic<std::size_t> worker_failures_{0};

    // Core owns the external lifecycle. Request coroutines only observe this
    // worker-local business state.
    bool prepared_{false};
    bool serve_requested_{false};

    runtime_completion worker_completion_;
};

}  // namespace ruvia::detail
