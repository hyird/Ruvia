#pragma once

#include "ruvia/web/App.h"

#include <mutex>
#include <optional>
#include <vector>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/Router.h"
#include "ruvia/web/detail/app/AppLifecycle.h"
#include "ruvia/web/detail/app/AppResource.h"
#include "ruvia/web/detail/app/DotenvInternal.h"
#include "ruvia/core/detail/NativePath.h"
#include "ruvia/web/detail/controller/ControllerDescriptors.h"
#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/redis/RedisInternal.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"

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
    ServerTopology topology;
    std::size_t workersPerListener;
    HttpServerOptions options{};
    std::optional<AppDocumentRootConfig> documentRootConfig;
    HttpErrorHandler errorHandler{nullptr};
    HttpNotFoundHandler notFoundHandler{nullptr};
    std::pmr::vector<AppHook> onStartHooks{appResource()};
    std::pmr::vector<AppHook> onStopHooks{appResource()};
#ifdef RUVIA_ENABLE_DATABASE
    std::pmr::vector<DbDefinition> databases{appResource()};
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::pmr::vector<RedisDefinition> redis{appResource()};
#endif
    Env env;
    ControllerStore controllerLifetimes;
    std::unique_ptr<Router, PmrObjectDeleter<Router>> router;
    std::unique_ptr<AppRuntimeGraph, PmrObjectDeleter<AppRuntimeGraph>> runtime;

    mutable std::mutex mutex;
    AppLifecycle lifecycle;
};

}  // namespace ruvia::detail
