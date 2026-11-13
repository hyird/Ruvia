#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_group.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/web_worker.h"

#include "http/static_root_index.h"
#include "http3/http3_endpoint.h"
#include "integration/worker_capabilities.h"
#include "router/route_table.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"
#include "server/http_server_worker_state.h"
#include "server/inbound_buffer_budget.h"
#include "server/windows_acceptor.h"
#include "server/worker_connections.h"

namespace ruvia {
class worker_ref;
}  // namespace ruvia

namespace ruvia::detail {

struct http_server_options;

// The application's one document-root refresher. The first registered worker
// walks the root, reads small files into the snapshot and precompresses changed files
// once per interval; every worker adopts the published immutable snapshot on
// its own thread. The runtime graph owns this object, so it outlives every
// worker and their joined refresh tasks.
class document_root_publisher final {
public:
    // Builds the initial snapshot; a failure here fails application startup.
    document_root_publisher(const std::filesystem::path& root, const static_root_config_storage& config,
        std::chrono::milliseconds interval, static_root_precompression_options precompression,
        static_root_resident_options resident, std::size_t worker_count);
    ~document_root_publisher();

    document_root_publisher(const document_root_publisher&) = delete;
    document_root_publisher& operator=(const document_root_publisher&) = delete;

    [[nodiscard]] std::chrono::milliseconds interval() const noexcept {
        return interval_;
    }

    [[nodiscard]] std::size_t failures() const noexcept {
        return failures_.load(std::memory_order_relaxed);
    }

    // Counts a failed refresh pass or adoption and reports its cause through
    // the rate-limited unhandled-failure sink; the last complete snapshot
    // stays published.
    void record_failure(std::exception_ptr failure) noexcept;

    // One rebuild pass on the refresher. A changed tree is published; a
    // failed pass is counted and the last complete snapshot stays published.
    [[nodiscard]] task<void> refresh(
        const worker_ref& worker, const http_server_worker_state& state, const stop_token& stop = {});

private:
    friend class static_root_runtime;

    using publication_owner =
        std::unique_ptr<document_root_snapshot>;

    struct mailbox final {
        ~mailbox();

        // Exchange transfers exclusive ownership of the node, not a borrowed
        // shared_ptr source. Exactly one exchanging thread reclaims each node.
        std::atomic<document_root_snapshot*> pending_{nullptr};
    };

    static_assert(std::atomic<document_root_snapshot*>::is_always_lock_free);

    // The coordinator registers every worker before starting any of them.
    // The fixed mailbox storage then outlives all workers and joined tasks.
    // The first mailbox identifies the one refresher.
    [[nodiscard]] mailbox& register_worker() noexcept;

    void publish(document_root_snapshot snapshot);
    void commit_publication(document_root_snapshot snapshot,
        std::vector<publication_owner>& publication) noexcept;

    // The configured path, not the snapshot's canonical one: a refresh must
    // re-resolve symlinks so a retargeted root (atomic deploy) is followed.
    std::filesystem::path root_;
    std::chrono::milliseconds interval_;
    static_root_precompression_options precompression_;
    static_root_resident_options resident_;
    // Only the first registered worker reads or replaces current_ after startup.
    document_root_snapshot current_;
    std::vector<mailbox> mailboxes_;
    std::size_t registered_workers_{0};
    std::atomic<std::size_t> failures_{0};
};

// One worker's adopted document-root generations. Request bindings lease the
// current generation with a plain counter; a superseded generation stays until
// its leases drain, so a long request pins only the snapshot it borrows. The
// worker joins refresh() before teardown.
class static_root_runtime final {
public:
    static_root_runtime(http_server_options& options, const worker_ref& worker_value,
        const http_server_worker_state& state_value);
    ~static_root_runtime();
    static_root_runtime(const static_root_runtime&) = delete;
    static_root_runtime& operator=(const static_root_runtime&) = delete;

    [[nodiscard]] bool enabled() const noexcept {
        return publisher_ != nullptr;
    }

    // Once per interval while the worker admits work: reclaim drained
    // generations, run the application's rebuild if this worker claimed it,
    // then adopt the published snapshot.
    [[nodiscard]] task<void> refresh(const stop_token& stop = {});

private:
    using generation_owner =
        std::unique_ptr<document_root_generation>;

    void adopt_published();

    http_server_options& options_;
    document_root_publisher* publisher_;
    document_root_publisher::mailbox* mailbox_;
    const worker_ref& worker_;
    const http_server_worker_state& state_;
    // Retained locally if preparing a worker generation fails; a later
    // publication supersedes it instead of accumulating missed snapshots.
    document_root_publisher::publication_owner pending_;
    generation_owner current_;
    std::vector<generation_owner> retired_;
    const bool refreshes_;
};

}  // namespace ruvia::detail

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
    void adopt_tls_identities(
        std::size_t listener_index, std::shared_ptr<const http_server_tls_identities> identities) noexcept {
        connections_.adopt_tls_identities(listener_index, std::move(identities));
    }
    // Owner thread only, from controls posted to runtime(): the listener
    // transactions of application::add_listener() and remove_listener()
    // (see worker_connections).
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition, prepared_http_server_session session
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory
#endif
    ) {
        connections_.prepare_listener(index, http3_index, definition, false, std::move(session)
#if defined(RUVIA_ENABLE_HTTP3)
                                                                                 ,
            directory
#endif
        );
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

#if defined(RUVIA_ENABLE_HTTP3)
#endif
#if defined(_WIN32)
#endif

namespace ruvia::detail {

struct app_worker_slot final {
    app_worker_slot(controller_store configured_controllers,
        std::unique_ptr<route_table> configured_routes,
        std::unique_ptr<web_worker_runtime> configured_runtime)
        : controllers_(std::move(configured_controllers)),
          routes_(std::move(configured_routes)),
          runtime_(std::move(configured_runtime)) {}

    app_worker_slot(app_worker_slot&&) noexcept = default;
    app_worker_slot& operator=(app_worker_slot&&) noexcept = default;
    app_worker_slot(const app_worker_slot&) = delete;
    app_worker_slot& operator=(const app_worker_slot&) = delete;

    // Destruction is intentionally reversed: runtime, routes, controllers.
    controller_store controllers_;
    std::unique_ptr<route_table> routes_;
    std::unique_ptr<web_worker_runtime> runtime_;
};

struct app_runtime_graph final {
    app_runtime_graph() = default;

    // Declared before workers so it is destroyed after suspended worker tasks.
    std::unique_ptr<blocking_pool> blocking_pool_;
    // The one document-root refresher; null without a document root. Workers
    // borrow it and adopt its snapshots, so it is declared before them.
    std::unique_ptr<document_root_publisher> document_root_;
    // Every worker's route_table binds its instances to this one immutable plan.
    compiled_route_plan_ptr_type route_plan_;
    // A non-routed run keeps its original empty plan alive if its first routed
    // listener is added while serving; a stopping worker may still borrow it.
    compiled_route_plan_ptr_type empty_route_plan_;
    bool routes_enabled_{false};
#if defined(RUVIA_ENABLE_HTTP3)
    // Hands QUIC datagrams to the worker that owns their connection; null
    // without an HTTP/3 listener. Declared before the workers and the
    // acceptor, whose queued hand-overs reference it.
    std::unique_ptr<http3_worker_directory> http3_directory_;
#endif
#if defined(_WIN32)
    // The one thread that owns every listener and hands sockets and datagrams
    // to the workers. Declared before them: they send their datagrams
    // through it until they joined, so it is destroyed after them.
    std::unique_ptr<windows_acceptor> acceptor_;
#endif
    std::vector<app_worker_slot> workers_;
    // The one lifecycle over every independently listening worker. It borrows
    // their core runtimes, so it is declared after them.
    worker_group group_;
    // Guarded by the application mutex. Worker handles are published only
    // once every worker is ready, so a posted job never meets a starting worker.
    bool workers_ready_{false};
    // Guarded by the application mutex. The TCP endpoint each listener slot
    // serves: slot i is listener i on every worker (and in the Windows
    // acceptor), nullopt once removed or while being added. Slots are never
    // reused, so a queued hand-over cannot reach another listener.
    std::vector<std::optional<asio::ip::tcp::endpoint>> listener_slots_;
    // HTTP/3 services are numbered alike, apart from the TCP listeners.
    std::size_t http3_slot_count_{0};
    // Where every TLS listener's ticket key epochs start, so a listener
    // added while the run serves rotates with the others.
    std::chrono::steady_clock::time_point tls_key_origin_{};
};

}  // namespace ruvia::detail
