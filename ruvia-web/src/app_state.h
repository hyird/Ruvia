#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <asio/ip/tcp.hpp>

#include "app.h"
#include "blocking_pool.h"
#include "callback_ref.h"
#include "controller.h"
#include "document_root_publisher.h"
#include "env.h"
#include "http_client_pool.h"
#include "http_response_body.h"
#include "http_server.h"
#include "request_stream.h"
#include "route_table.h"
#include "runtime_control.h"
#include "server_connection.h"
#include "server_listener.h"
#include "server_observation.h"
#include "server_options.h"
#include "static_root_index.h"
#include "web_worker_runtime.h"
#include "worker_runtime.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "db_pool.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis_pool.h"
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include "http3_endpoint.h"
#endif
#if defined(_WIN32)
#include "windows_acceptor.h"
#endif

// The application singleton's state: its configuration, lifecycle state
// machine and the runtime graph of one run.
namespace ruvia::detail {

enum class app_lifecycle_state : std::uint8_t {
    stopped,
    preparing,
    starting,
    running,
    stopping,
};

enum class app_stop_request : std::uint8_t {
    ignored,
    // Startup was in progress: the runtime stops at once, rolling it back.
    startup,
    // The app was serving: the run() owner stops admission, then the runtime.
    serving,
};

// application owns the mutex protecting this state machine. application::run() is the sole
// lifecycle owner; other threads may only request the monotonic transition to
// stopping. A stop requested while stopped is latched and consumed by the next run.
class app_lifecycle final {
public:
    [[nodiscard]] bool active() const noexcept {
        return state_ != app_lifecycle_state::stopped;
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ == app_lifecycle_state::stopping;
    }

    // Serving, with no stop requested yet.
    [[nodiscard]] bool running() const noexcept {
        return state_ == app_lifecycle_state::running;
    }

    [[nodiscard]] bool begin_run() noexcept {
        if (state_ != app_lifecycle_state::stopped) {
            return false;
        }
        state_ = stop_pending_ ? app_lifecycle_state::stopping : app_lifecycle_state::preparing;
        stop_pending_ = false;
        return true;
    }

    [[nodiscard]] bool publish_runtime() noexcept {
        if (state_ != app_lifecycle_state::preparing) {
            return false;
        }
        state_ = app_lifecycle_state::starting;
        return true;
    }

    [[nodiscard]] bool mark_running() noexcept {
        if (state_ != app_lifecycle_state::starting) {
            return false;
        }
        state_ = app_lifecycle_state::running;
        return true;
    }

    [[nodiscard]] app_stop_request request_stop() noexcept {
        switch (state_) {
            case app_lifecycle_state::preparing:
            case app_lifecycle_state::starting:
                state_ = app_lifecycle_state::stopping;
                return app_stop_request::startup;
            case app_lifecycle_state::running:
                state_ = app_lifecycle_state::stopping;
                return app_stop_request::serving;
            case app_lifecycle_state::stopped:
                stop_pending_ = true;
                return app_stop_request::ignored;
            case app_lifecycle_state::stopping:
                return app_stop_request::ignored;
        }
        return app_stop_request::ignored;
    }

    void complete_run() noexcept {
        state_ = app_lifecycle_state::stopped;
    }

private:
    app_lifecycle_state state_{app_lifecycle_state::stopped};
    bool stop_pending_{};
};

struct app_runtime_graph;

struct app_document_root_config final {
    explicit app_document_root_config(static_root_config_storage configured_static_options) :
        static_options_(std::move(configured_static_options)) {}

    http_native_path_string_type root_;
    static_root_config_storage static_options_;
    std::chrono::milliseconds refresh_interval_{};
    static_root_precompression_options precompression_;
    static_root_resident_options resident_;
};

struct app_state final {
    app_state();
    ~app_state();

    std::vector<http_server_listener_definition> listeners_;
    callback_registration<access_log_callback> access_log_callback_;
    callback_registration<connection_failure_callback> connection_failure_callback_;
    callback_registration<tls_client_hello_callback> tls_client_hello_callback_;
    callback_registration<http2_input_callback> http2_input_callback_;
#if defined(RUVIA_ENABLE_HTTP2)
    callback_registration<http2_frame_callback> http2_frame_callback_;
    callback_registration<http2_headers_callback> http2_headers_callback_;
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    callback_registration<http3_frame_callback> http3_frame_callback_;
    callback_registration<http3_field_callback> http3_field_callback_;
#endif
    callback_registration<connection_admission_callback> connection_admission_callback_;
    callback_registration<connection_open_callback> connection_open_callback_;
    callback_registration<connection_close_callback> connection_close_callback_;
    callback_registration<connection_callback> connection_callback_;
    callback_registration<request_stream_callback> request_stream_callback_;
    http_server_options options_{};
    std::optional<app_document_root_config> document_root_config_;
    app_configuration configuration_;
    std::optional<blocking_pool_options> blocking_pool_{std::in_place};
    std::vector<callback_registration<app_hook>> on_start_hooks_;
    std::vector<callback_registration<app_hook>> on_stop_hooks_;
#ifdef RUVIA_ENABLE_DATABASE
    std::vector<db_definition> databases_;
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::vector<redis_definition_type> redis_;
#endif
    std::vector<http_client_definition_type> http_clients_;
    env env_;
    std::unique_ptr<app_runtime_graph> runtime_;

    mutable std::mutex mutex_;
    std::condition_variable lifecycle_changed_;
    app_lifecycle lifecycle_;
    // Controls never hold a mutex across suspension. The synchronous run()
    // owner keeps the published runtime and frozen configuration alive until
    // every admitted control (including its blocking preparation) and every
    // startup stop rollback in progress has released its count.
    runtime_control_gate runtime_control_gate_;
    std::size_t runtime_controls_{};
};

inline void ensure_app_not_running(bool running, const char* message) {
    if (running) {
        throw std::logic_error(message);
    }
}

template <typename configure_type>
application& mutate_stopped_app(application& app, app_state& state_value, const char* running_message, configure_type&& configure) {
    std::lock_guard lock(state_value.mutex_);
    ensure_app_not_running(state_value.lifecycle_.active(), running_message);
    std::forward<configure_type>(configure)(state_value);
    return app;
}

void run_app(application& app, app_state& state_value);
// The one stop chain for a published runtime: closes admission on the acceptor
// and every worker, synchronously running their stop callbacks on the calling
// thread. The caller must not hold app_state::mutex_ and must pin `runtime`
// through app_state::runtime_controls_; run_app owns retirement.
void request_runtime_stop(app_runtime_graph& runtime) noexcept;

struct app_worker_slot final {
    app_worker_slot(controller_store configured_controllers, std::unique_ptr<route_table> configured_routes, std::unique_ptr<web_worker_runtime> configured_runtime) :
        controllers_(std::move(configured_controllers)),
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
