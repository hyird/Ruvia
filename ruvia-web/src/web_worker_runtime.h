#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "blocking_pool.h"
#include "connection_lifecycle.h"
#include "context_access.h"
#include "document_root_publisher.h"
#include "http_server.h"
#include "inbound_buffer.h"
#include "move_only_function.h"
#include "rate_limit.h"
#include "response_coding_policy.h"
#include "route_table.h"
#include "server_listener.h"
#include "server_options.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "tls_server.h"
#include "worker_capabilities.h"
#include "worker_connections.h"
#include "worker_ref.h"
#include "worker_runtime.h"
#include "worker_signal.h"
#include "worker_state.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include "http3_endpoint.h"
#endif
#if defined(_WIN32)
#include "windows_acceptor.h"
#endif

// One server worker's runtime and the endpoint behind its web_worker_ref.
namespace ruvia::detail {

class worker_state_registry;

// The endpoint behind every web_worker_ref. Core owns admission: posts go
// through worker_ref::post_payload and accepted jobs start only while that
// worker accepts and its stop token is clear. Started jobs run in the worker's
// task_scope, which the server worker joins before retiring capabilities.
//
// Escaped handles share this object so their terminal queries (valid,
// accepting, id, stats) remain safe after the server is gone. The borrowed
// server state below is touched only by jobs started on the worker; core
// abandons every other accepted job without running it.
class web_worker_dispatch final {
public:
    using job_type = move_only_function<ruvia::task<void>(web_worker_context&)>;

    web_worker_dispatch(ruvia::worker_runtime& runtime, worker_capabilities& capabilities, task_scope& jobs, const stop_token& stop_token_value);

    web_worker_dispatch(const web_worker_dispatch&) = delete;
    web_worker_dispatch& operator=(const web_worker_dispatch&) = delete;

    [[nodiscard]] static web_worker_ref make_ref(std::shared_ptr<web_worker_dispatch> dispatch) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    [[nodiscard]] web_worker_post_result post(job_type job);
    [[nodiscard]] web_worker_stats stats() const noexcept;

    // Reconciles the outstanding reservation of an accepted job that core
    // destroys without running. Public only for the reservation deleter.
    void abandon() noexcept;

private:
    void start(job_type job);
    [[nodiscard]] ruvia::task<void> run(job_type job);

    // A stable copy: queries stay valid after the server's runtime is gone.
    worker_ref worker_;
    ruvia::worker_runtime& runtime_;
    worker_client_registry_view client_registries_;
    const worker_state_registry& worker_states_;
    task_scope& jobs_;
    const stop_token& stop_token_;
    std::atomic_uint64_t accepted_{0};
    std::atomic_uint64_t queue_full_{0};
    std::atomic_uint64_t worker_stopping_{0};
    std::atomic_uint64_t completed_{0};
    std::atomic_uint64_t failed_count_{0};
    std::atomic_size_t outstanding_{0};
};

class validated_http_server_configuration;
class web_worker_dispatch;

// One server worker: its core worker_runtime plus the HTTP domain state that
// runtime's hooks compose. Core owns the generic lifecycle (launch, ready/serve
// barrier, stop admission, finalization, join, first failure); application
// sequences every independently listening worker through one core worker_group.
class web_worker_runtime final {
public:
    // `pool` is the application pool the worker's event loop borrows for
    // every offload and client host lookup; null when the application
    // disabled it. It must outlive the worker. `worker_index` is this
    // worker's position among the application's workers.
    web_worker_runtime(const validated_http_server_configuration& configuration, const route_table& routes_value, worker_capability_definitions capabilities, blocking_pool* pool, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_placement http3_placement
#endif
    );
    ~web_worker_runtime();

    web_worker_runtime(const web_worker_runtime&) = delete;
    web_worker_runtime& operator=(const web_worker_runtime&) = delete;

    [[nodiscard]] ruvia::worker_runtime& runtime() & noexcept {
        return runtime_;
    }
    ruvia::worker_runtime& runtime() && = delete;
    [[nodiscard]] stop_token cancellation() const noexcept {
        return stop_token_;
    }
    // Safe from any thread, at any point in the lifecycle.
    [[nodiscard]] http_server_stats stats() const;
    [[nodiscard]] std::shared_ptr<connection_count> listener_connections(const asio::ip::tcp::endpoint& endpoint) const {
        return connections_.listener_connections(endpoint);
    }
    [[nodiscard]] web_worker_ref web_worker() const;
    // Owner thread, before admitting this run's first routed listener.
    void adopt_route_rate_limiter(rate_limiter& prepared) noexcept {
        capabilities_.adopt_route_rate_limiter(prepared);
    }
    // Owner thread only, from a control posted to runtime(): listener
    // `listener_index` -- TLS over TCP and its HTTP/3 service alike --
    // selects every later handshake's identity from `identities`.
    void adopt_tls_identities(std::size_t listener_index, std::shared_ptr<const http_server_tls_identities> identities) noexcept {
        connections_.adopt_tls_identities(listener_index, std::move(identities));
    }
    // Owner thread only, from controls posted to runtime(): the listener
    // transactions of application::add_listener() and remove_listener()
    // (see worker_connections).
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index, const http_server_listener_definition& definition, prepared_http_server_session session
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory
#endif
    ) {
#if defined(RUVIA_ENABLE_HTTP3)
        connections_.prepare_listener(index, http3_index, definition, false, std::move(session), directory);
#else
        connections_.prepare_listener(index, http3_index, definition, false, std::move(session));
#endif
    }
    void open_listener(std::size_t index) noexcept {
        connections_.open_listener(index);
    }
    void discard_listener(std::size_t index, std::shared_ptr<void> receipt = {}) noexcept {
        connections_.discard_listener(index, std::move(receipt));
    }
    void remove_listener(std::size_t index, std::shared_ptr<void> receipt, listener_remove_options options = {}) noexcept {
        connections_.remove_listener(index, std::move(receipt), options);
    }
    [[nodiscard]] bool close_connection(std::uint64_t connection_id) noexcept {
        return connections_.close_connection(connection_id);
    }
#if !defined(_WIN32)
    [[nodiscard]] listener_socket_duplicates duplicate_listener(std::size_t index) {
        return connections_.duplicate_listener(index);
    }
#endif
#if defined(_WIN32)
    // This worker as the Windows acceptor's hand-over target.
    [[nodiscard]] windows_acceptor::business_worker acceptor_target() noexcept {
        return connections_.acceptor_target();
    }
#endif

private:
    web_worker_runtime(std::span<const http_server_listener_definition> listeners, const route_table& routes_value, worker_capability_definitions capabilities, http_server_options validated_options, blocking_pool* pool, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_placement http3_placement
#endif
    );

    void serve_on_context();
    void stop_admission_on_context() noexcept;
    void finalize_on_context() noexcept;
    task<void> run_worker();
    ruvia::worker_runtime runtime_;
    asio::io_context& io_context_;
    worker_signal finalize_signal_;
    stop_source stop_source_;
    stop_token stop_token_{stop_source_.token()};
    const route_table& routes_;
    // Every connection's inbound buffers charge this worker-wide budget.
    inbound_buffer_budget inbound_budget_;
    // Borrowed through options_.codecs_ by requests and connections, which
    // all end before it is destroyed.
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
