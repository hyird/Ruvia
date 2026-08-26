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

namespace ruvia::detail {

struct AppRuntimeGraph;
struct AppState;

void applyServerConfig(AppState& state, const ServerConfig& config);

struct AppStaticMimeType final {
    explicit AppStaticMimeType(std::pmr::memory_resource* resource)
        : extension(resource),
          contentType(resource) {}

    std::pmr::string extension;
    std::pmr::string contentType;
};

struct AppStaticRootOptions final {
    explicit AppStaticRootOptions(std::pmr::memory_resource* resource)
        : cacheControl(resource),
          indexFile(resource),
          defaultContentType("application/octet-stream", resource),
          mimeTypes(resource),
          fileTypeExtensions(resource) {}

    std::pmr::string cacheControl;
    std::pmr::string indexFile;
    std::pmr::string defaultContentType;
    std::pmr::vector<AppStaticMimeType> mimeTypes;
    StaticFileTypePolicy::Kind fileTypeKind{StaticFileTypePolicy::Kind::kDefaults};
    std::pmr::vector<std::pmr::string> fileTypeExtensions;
    StaticRangeRequestPolicy rangeRequests{StaticRangeRequestPolicy::kHonor};
    StaticResponseValidatorPolicy responseValidators{StaticResponseValidatorPolicy::kEmit};
    StaticDotfilePolicy dotfiles{StaticDotfilePolicy::kDeny};
};

struct AppDocumentRootConfig final {
    explicit AppDocumentRootConfig(std::pmr::memory_resource* resource)
        : root(resource),
          staticOptions(resource) {}

    NativePathString root;
    AppStaticRootOptions staticOptions;
    DocumentRootRuntimeConfig runtime;
    StaticRootPrecompressionOptions precompression;
};

struct AppListenerConfig final {
    AppListenerConfig(std::pmr::memory_resource* resource, std::string_view configuredAddress,
        std::uint16_t configuredPort, HttpServerListenerDefinition::PlainHttp)
        : address(configuredAddress, resource),
          port(configuredPort),
          transport(std::in_place_type<HttpServerListenerDefinition::PlainHttp>) {}

    AppListenerConfig(std::pmr::memory_resource* resource, std::string_view configuredAddress,
        std::uint16_t configuredPort, HttpServerListenerDefinition::Tls configuredTransport)
        : address(configuredAddress, resource),
          port(configuredPort),
          transport(std::in_place_type<HttpServerListenerDefinition::Tls>,
              std::move(configuredTransport)) {}

    AppListenerConfig(std::pmr::memory_resource* resource, std::string_view configuredAddress,
        std::uint16_t configuredPort,
        HttpServerListenerDefinition::RedirectHttpToHttps configuredTransport)
        : address(configuredAddress, resource),
          port(configuredPort),
          transport(std::in_place_type<HttpServerListenerDefinition::RedirectHttpToHttps>,
              configuredTransport) {}

    std::pmr::string address;
    std::uint16_t port;
    HttpServerListenerDefinition::Transport transport;
};

struct AppState final {
    AppState();
    ~AppState();

    std::pmr::vector<AppListenerConfig> listeners{appResource()};
    std::size_t workerCount{0};
    ProcessSignalHandlerPolicy processSignalHandlers{ProcessSignalHandlerPolicy::kExternalOwner};
    AccessLogCallback accessLogCallback;
    ConnectionFailureCallback connectionFailureCallback;
    HttpServerOptions options{};
    std::optional<AppDocumentRootConfig> documentRootConfig;
    HttpErrorHandler errorHandler{nullptr};
    HttpNotFoundHandler notFoundHandler{nullptr};
    std::pmr::vector<std::pair<std::pmr::string, HttpErrorHandler>> prefixErrorHandlers{
        appResource()};
    std::pmr::vector<std::pair<std::pmr::string, HttpNotFoundHandler>> prefixNotFoundHandlers{
        appResource()};
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
