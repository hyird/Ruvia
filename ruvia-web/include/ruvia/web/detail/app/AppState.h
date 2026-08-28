#pragma once

#include "ruvia/web/App.h"

#include <cstddef>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/app/AppLifecycle.h"
#include "ruvia/web/detail/app/AppResource.h"
#include "ruvia/core/detail/util/NativePath.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/detail/db/DbConfigStorage.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/detail/redis/RedisConfigStorage.h"
#endif
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/HttpServerListener.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/http/static/StaticRootConfigStorage.h"

namespace ruvia::detail {

struct AppRuntimeGraph;
struct AppState;

void applyServerConfig(AppState& state, const ServerConfig& config);

struct AppDocumentRootConfig final {
    AppDocumentRootConfig(std::pmr::memory_resource* resource, StaticRootConfigStorage configuredStaticOptions)
        : root(resource),
          staticOptions(std::move(configuredStaticOptions)) {}

    NativePathString root;
    StaticRootConfigStorage staticOptions;
    DocumentRootRuntimeConfig runtime;
    StaticRootPrecompressionOptions precompression;
};

struct AppState final {
    AppState();
    ~AppState();

    std::pmr::vector<HttpServerListenerDefinition> listeners{appResource()};
    std::size_t workerCount{0};
    ProcessSignalHandlerPolicy processSignalHandlers{ProcessSignalHandlerPolicy::kExternalOwner};
    AccessLogCallback accessLogCallback;
    ConnectionFailureCallback connectionFailureCallback;
    HttpServerOptions options{};
    std::optional<AppDocumentRootConfig> documentRootConfig;
    HttpErrorHandler errorHandler{nullptr};
    HttpNotFoundHandler notFoundHandler{nullptr};
    std::pmr::vector<std::pair<std::pmr::string, HttpErrorHandler>> prefixErrorHandlers{appResource()};
    std::pmr::vector<std::pair<std::pmr::string, HttpNotFoundHandler>> prefixNotFoundHandlers{appResource()};
    std::pmr::vector<ControllerMiddlewareDescriptor> globalMiddlewares{appResource()};
    std::pmr::vector<WorkerStateDefinition> workerStates{appResource()};
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
