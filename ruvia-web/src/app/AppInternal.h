#pragma once

#include "ruvia/app/App.h"

#include <mutex>
#include <optional>
#include <vector>

#include "detail/HttpPmrObject.h"
#include "ruvia/router/Router.h"
#include "AppResource.h"
#include "DotenvInternal.h"
#include "ruvia/detail/NativePath.h"
#include "ruvia/http/ControllerDescriptors.h"
#include "db/DbInternal.h"
#include "client/HttpClientInternal.h"
#include "redis/RedisInternal.h"

namespace ruvia::detail {

struct AppRuntimeGraph;

struct AppDocumentRootConfig final {
    explicit AppDocumentRootConfig(std::pmr::memory_resource* resource)
        : root(resource) {}

    NativePathString root;
    StaticRootOptions staticOptions;
};

struct AppState final {
    AppState();
    ~AppState();

    std::pmr::string listenAddress{appResource()};
    std::optional<std::uint16_t> httpListenPort{8080};
    std::optional<std::uint16_t> httpsListenPort;
    bool autoHttps{false};
    std::size_t threadNum;
    HttpServerOptions options{};
    std::optional<AppDocumentRootConfig> documentRootConfig;
    MemoryPoolConfig memoryConfig{};
    HttpErrorHandler errorHandler{nullptr};
    HttpNotFoundHandler notFoundHandler{nullptr};
    std::pmr::vector<AppHook> onStartHooks{appResource()};
    std::pmr::vector<AppHook> onStopHooks{appResource()};
#ifdef RUVIA_ENABLE_MARIADB
    std::pmr::vector<DbDefinition> databases{appResource()};
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::pmr::vector<RedisDefinition> redis{appResource()};
#endif
    std::pmr::vector<HttpClientDefinition> httpClients{appResource()};

    Env env;
    ControllerStore controllerLifetimes;
    std::unique_ptr<AppRuntimeGraph, HttpPmrObjectDeleter<AppRuntimeGraph>> runtime;
    Router router;

    mutable std::mutex mutex;
    bool autoControllersLoaded{false};
    bool running{false};
    // Set by stop() (including from the signal handler) so run()'s worker-start
    // loop can observe a shutdown requested mid-startup and tear down the workers
    // it started -- otherwise a stop() that lands before a worker is started is a
    // no-op on that worker and run()'s join would hang. Reset under the lock at
    // the top of run() because a completed run()/stop() cycle leaves it true.
    bool stopRequested{false};
};

}  // namespace ruvia::detail
