#pragma once

#include "ruvia/app/App.h"

#include <mutex>
#include <optional>
#include <vector>

#include "ruvia/memory/PmrObject.h"
#include "ruvia/router/Router.h"
#include "DotenvInternal.h"
#include "ruvia/detail/NativePath.h"
#include "ruvia/http/ControllerDescriptors.h"
#include "../db/DbInternal.h"
#include "../http/client/HttpClientInternal.h"
#include "../redis/RedisInternal.h"

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

    std::pmr::string listenAddress{ProcessMemory::instance().upstreamResource()};
    std::optional<std::uint16_t> httpListenPort{8080};
    std::optional<std::uint16_t> httpsListenPort;
    bool autoHttps{false};
    std::size_t threadNum;
    HttpServerOptions options{};
    std::optional<AppDocumentRootConfig> documentRootConfig;
    MemoryPoolConfig memoryConfig{};
    HttpErrorHandler errorHandler{nullptr};
    std::pmr::vector<AppHook> onStartHooks{ProcessMemory::instance().upstreamResource()};
    std::pmr::vector<AppHook> onStopHooks{ProcessMemory::instance().upstreamResource()};
    std::pmr::vector<ControllerMiddlewareDescriptor> globalMiddlewares{
        ProcessMemory::instance().upstreamResource()};
#ifdef RUVIA_ENABLE_MARIADB
    std::pmr::vector<DbDefinition> databases{ProcessMemory::instance().upstreamResource()};
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::pmr::vector<RedisDefinition> redis{ProcessMemory::instance().upstreamResource()};
#endif
#ifdef RUVIA_ENABLE_HTTP_CLIENT
    std::pmr::vector<HttpClientDefinition> httpClients{ProcessMemory::instance().upstreamResource()};
#endif

    Env env;
    ControllerStore controllerLifetimes;
    std::unique_ptr<AppRuntimeGraph, PmrObjectDeleter<AppRuntimeGraph>> runtime;
    Router router;

    mutable std::mutex mutex;
    bool autoControllersLoaded{false};
    bool routeGraphFinalized{false};
    bool running{false};
};

}  // namespace ruvia::detail
