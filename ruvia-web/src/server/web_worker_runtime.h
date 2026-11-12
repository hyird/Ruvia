#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <asio/io_context.hpp>

#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/web/web_worker.h"

#include "integration/worker_capabilities.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"
#include "server/http_server_worker_state.h"
#include "server/inbound_buffer_resource.h"
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
// sequences every independently listening worker through one core worker_group.
class web_worker_runtime final {
public:
    // `pool` is the application pool the worker's event loop borrows for
    // every offload and client host lookup; null when the application
    // disabled it. It must outlive the worker. `worker_index` is this
    // worker's position among the application's workers.
    web_worker_runtime(const validated_http_server_configuration& configuration, const route_table& routes_value,
        worker_capability_definitions capabilities, blocking_pool* pool, std::size_t worker_index
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
    // Safe from any thread, at any point in the lifecycle.
    [[nodiscard]] http_server_stats stats() const noexcept;
    [[nodiscard]] web_worker_handle web_worker() const;
    // Owner thread, before admitting this run's first routed listener.
    void adopt_route_rate_limiter(rate_limiter& prepared) noexcept {
        capabilities_.adopt_route_rate_limiter(prepared);
    }
    // Owner thread only, from a control posted to runtime(): listener
    // `listener_index` -- TLS over TCP and its HTTP/3 service alike --
    // selects every later handshake's identity from `identities`.
    void adopt_tls_identities(
        std::size_t listener_index, std::shared_ptr<const http_server_tls_identities> identities) noexcept {
        connections_.adopt_tls_identities(listener_index, std::move(identities));
    }
    // Owner thread only, from controls posted to runtime(): the listener
    // transactions of application::add_listener() and remove_listener()
    // (see worker_connections).
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory
#endif
    ) {
        connections_.prepare_listener(index, http3_index, definition, false
#if defined(RUVIA_ENABLE_HTTP3)
            ,
            directory
#endif
        );
    }
    void open_listener(std::size_t index) noexcept {
        connections_.open_listener(index);
    }
    void discard_listener(std::size_t index) noexcept {
        connections_.discard_listener(index);
    }
    void remove_listener(std::size_t index, std::shared_ptr<void> receipt) noexcept {
        connections_.remove_listener(index, std::move(receipt));
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
    web_worker_runtime(std::span<const http_server_listener_definition> listeners,
        const route_table& routes_value, worker_capability_definitions capabilities,
        http_server_options validated_options, blocking_pool* pool, std::size_t worker_index
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
