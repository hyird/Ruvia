#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/NativePath.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/App.h"

#include "app/AppLifecycle.h"
#include "app/AppResource.h"
#include "router/Router.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "db/DbConfigStorage.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis/RedisConfigStorage.h"
#endif
#include "client/HttpClientConfigStorage.h"
#include "http/StaticRootConfigStorage.h"
#include "server/HttpServerListener.h"
#include "server/HttpServerOptions.h"

namespace ruvia::detail {

struct AppRuntimeGraph;
struct AppState;

void applyServerConfig(AppState& state, const server_config& config);

struct AppDocumentRootConfig final {
    AppDocumentRootConfig(
        std::pmr::memory_resource* resource, StaticRootConfigStorage configuredStaticOptions)
        : root(resource),
          staticOptions(std::move(configuredStaticOptions)) {}

    ruvia::NativePathString root;
    StaticRootConfigStorage staticOptions;
    DocumentRootRuntimeConfig runtime;
    StaticRootPrecompressionOptions precompression;
};

struct AppState final {
    AppState();
    ~AppState();

    std::pmr::vector<HttpServerListenerDefinition> listeners{appResource()};
    std::size_t worker_count{0};
    process_signal_handler_policy process_signal_handlers{process_signal_handler_policy::external_owner};
    AccessLogCallback accessLogCallback;
    ConnectionFailureCallback connectionFailureCallback;
    HttpServerOptions options{};
    std::optional<AppDocumentRootConfig> documentRootConfig;
    app_configuration configuration{appResource()};
    std::optional<BlockingPoolOptions> blockingPool{std::in_place};
    std::pmr::vector<AppHook> onStartHooks{appResource()};
    std::pmr::vector<AppHook> onStopHooks{appResource()};
#ifdef RUVIA_ENABLE_DATABASE
    std::pmr::vector<DbDefinition> databases{appResource()};
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::pmr::vector<RedisDefinition> redis{appResource()};
#endif
    std::pmr::vector<HttpClientDefinition> httpClients{appResource()};
    Env env;
    std::unique_ptr<AppRuntimeGraph, PmrObjectDeleter<AppRuntimeGraph>> runtime;

    mutable std::mutex mutex;
    std::condition_variable lifecycleChanged;
    AppLifecycle lifecycle;
};

}  // namespace ruvia::detail
