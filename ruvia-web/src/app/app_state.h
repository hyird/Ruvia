#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/native_path.h"
#include "ruvia/web/app.h"

#include "app/app_lifecycle.h"
#include "app/app_resource.h"
#include "router/router.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "db/db_config_storage.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis/redis_config_storage.h"
#endif
#include "client/http_client_config_storage.h"
#include "http/static_root_config_storage.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"

namespace ruvia::detail {

struct app_runtime_graph;
struct app_state;

void apply_server_config(app_state& state_value, const server_config& config);

struct app_document_root_config final {
    app_document_root_config(
        std::pmr::memory_resource* resource, static_root_config_storage configured_static_options)
        : root_(resource),
          static_options_(std::move(configured_static_options)) {}

    ruvia::native_path_string_type root_;
    static_root_config_storage static_options_;
    document_root_runtime_config runtime_;
    static_root_precompression_options precompression_;
};

struct app_state final {
    app_state();
    ~app_state();

    std::pmr::vector<http_server_listener_definition> listeners_{app_resource()};
    std::size_t worker_count_{0};
    process_signal_handler_policy process_signal_handlers_{process_signal_handler_policy::external_owner};
    access_log_callback_type access_log_callback_;
    connection_failure_callback_type connection_failure_callback_;
    http_server_options options_{};
    std::optional<app_document_root_config> document_root_config_;
    app_configuration configuration_{app_resource()};
    std::optional<blocking_pool_options> blocking_pool_{std::in_place};
    std::pmr::vector<app_hook_type> on_start_hooks_{app_resource()};
    std::pmr::vector<app_hook_type> on_stop_hooks_{app_resource()};
#ifdef RUVIA_ENABLE_DATABASE
    std::pmr::vector<db_definition> databases_{app_resource()};
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::pmr::vector<redis_definition_type> redis_{app_resource()};
#endif
    std::pmr::vector<http_client_definition_type> http_clients_{app_resource()};
    env env_;
    std::unique_ptr<app_runtime_graph, pmr_object_deleter<app_runtime_graph>> runtime_;

    mutable std::mutex mutex_;
    std::condition_variable lifecycle_changed_;
    app_lifecycle lifecycle_;
};

}  // namespace ruvia::detail
