#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_native_path.h"
#include "ruvia/web/app.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "db/db_config_storage.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis/redis_config_storage.h"
#endif
#include "client/http_client_config_storage.h"
#include "http/static_root_config_storage.h"
#include "http/static_root_index.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"

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
    app_document_root_config(
        std::pmr::memory_resource* resource, static_root_config_storage configured_static_options)
        : root_(resource),
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

    std::pmr::vector<http_server_listener_definition> listeners_{process_resource()};
    access_log_callback access_log_callback_;
    connection_failure_callback connection_failure_callback_;
    http_server_options options_{};
    std::optional<app_document_root_config> document_root_config_;
    app_configuration configuration_;
    std::optional<blocking_pool_options> blocking_pool_{std::in_place};
    std::pmr::vector<app_hook> on_start_hooks_{process_resource()};
    std::pmr::vector<app_hook> on_stop_hooks_{process_resource()};
#ifdef RUVIA_ENABLE_DATABASE
    std::pmr::vector<db_definition> databases_{process_resource()};
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::pmr::vector<redis_definition_type> redis_{process_resource()};
#endif
    std::pmr::vector<http_client_definition_type> http_clients_{process_resource()};
    env env_;
    std::unique_ptr<app_runtime_graph, pmr_object_deleter<app_runtime_graph>> runtime_;

    mutable std::mutex mutex_;
    std::condition_variable lifecycle_changed_;
    app_lifecycle lifecycle_;
};

inline void ensure_app_not_running(bool running, const char* message) {
    if (running) {
        throw std::logic_error(message);
    }
}

template <typename configure_type>
application& mutate_stopped_app(
    application& app, app_state& state_value, const char* running_message, configure_type&& configure) {
    std::lock_guard lock(state_value.mutex_);
    ensure_app_not_running(state_value.lifecycle_.active(), running_message);
    std::forward<configure_type>(configure)(state_value);
    return app;
}

void run_app(application& app, app_state& state_value);
// The one stop chain for a published runtime: closes admission on the acceptor
// and every worker. The caller holds state_value.mutex_; run_app owns retirement.
void request_runtime_stop(app_state& state_value) noexcept;

}  // namespace ruvia::detail
