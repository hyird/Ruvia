#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <span>

#include <asio/io_context.hpp>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_submission_view.h"
#include "ruvia/web/web_worker.h"

#include "integration/worker_capabilities.h"
#include "server/connection_scanner.h"
#include "server/http_connection_state.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"
#include "server/http_server_worker_state.h"
#include "server/inbound_buffer_resource.h"
#include "server/native_accepted_socket_ticket.h"
#include "server/static_root_runtime.h"
#include "server/worker_codecs.h"
#include "server/worker_connections.h"

namespace ruvia::detail {

class route_table;
class validated_http_server_configuration;
class web_worker_dispatch;

// One server worker: its core worker_runtime plus the HTTP domain state that
// runtime's hooks compose. Core owns the generic lifecycle (launch, ready/serve
// barrier, stop admission, finalization, join, first failure); application
// sequences every worker and the acceptor through one core worker_group.
class web_worker_runtime final {
public:
    web_worker_runtime(const validated_http_server_configuration& configuration,
        const route_table& routes_value, worker_capability_definitions capabilities);
    ~web_worker_runtime();

    web_worker_runtime(const web_worker_runtime&) = delete;
    web_worker_runtime& operator=(const web_worker_runtime&) = delete;

    [[nodiscard]] ruvia::worker_runtime& runtime() & noexcept {
        return runtime_;
    }
    ruvia::worker_runtime& runtime() && = delete;
    // Lock-free admission snapshot for the acceptor. A true
    // result is advisory; the worker rechecks capacity when the socket is delivered.
    [[nodiscard]] bool available_for_network_dispatch() const noexcept;
    // Called on the worker after bounded acceptor queue delivery. Rechecks worker state
    // and capacity, assigns before any I/O, and consumes the ticket on every path.
    void accept_transferred_connection(native_accepted_socket_ticket&& ticket) noexcept;
    // Any thread, while serving: stops admitting connections and requests,
    // closes connections outside a request exchange, and lets in-flight
    // requests finish before their connections close. Stopping the runtime
    // later closes whatever is left.
    void request_drain() noexcept;
    // Lifecycle thread: false when `deadline` passed before the drain completed.
    // A worker that never served, or that stopped, has nothing to drain.
    [[nodiscard]] bool wait_until_drained(std::chrono::steady_clock::time_point deadline);
    // Safe from any thread, at any point in the lifecycle.
    [[nodiscard]] http_server_stats stats() const noexcept;
    [[nodiscard]] worker_submission_view network_submission() const& noexcept {
        return worker_runtime_.submission();
    }
    worker_submission_view network_submission() const&& = delete;
    [[nodiscard]] web_worker_handle web_worker() const;

private:
    web_worker_runtime(std::span<const http_server_listener_definition> listeners,
        const route_table& routes_value, worker_capability_definitions capabilities,
        http_server_options validated_options);

    void serve_on_context();
    void drain_on_context() noexcept;
    void stop_admission_on_context() noexcept;
    void finalize_on_context() noexcept;
    task<void> run_worker();
    ruvia::worker_runtime runtime_;
    asio::io_context& io_context_;
    ruvia::worker_runtime_context& worker_runtime_;
    worker_signal finalize_signal_;
    stop_source stop_source_;
    stop_token stop_token_{stop_source_.token()};
    const route_table& routes_;
    worker_memory memory_;
    // Every connection's inbound buffers charge this worker-wide budget.
    inbound_buffer_budget inbound_budget_;
    // Allocates from memory_ and is borrowed through options_.codecs_ by
    // requests and connections, which all end before it is destroyed.
    worker_codecs codecs_;
    task_scope background_tasks_;
    http_server_worker_state worker_state_{http_server_worker_state::fresh};
    http_server_options options_;
    static_root_runtime static_roots_;
    worker_capabilities capabilities_;
    worker_connections connections_;
    // Shared only with escaped public handles: their terminal queries outlive
    // this worker. Jobs run in background_tasks_ and are joined by run_worker.
    std::shared_ptr<web_worker_dispatch> web_worker_dispatch_;
    // Atomic because stats() reads them from the caller's thread while this
    // worker updates them. Relaxed: they are counters, and publish nothing.
    std::atomic<std::size_t> worker_failures_{0};
};

}  // namespace ruvia::detail
