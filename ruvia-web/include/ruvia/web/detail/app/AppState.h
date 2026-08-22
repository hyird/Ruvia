#pragma once

#include "ruvia/web/App.h"

#include <atomic>
#include <cstddef>
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

namespace ruvia::detail {

struct AppRuntimeGraph;

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
    bool enableRanges{true};
    bool enableValidators{true};
    bool serveDotfiles{false};
};

// App::stop() may cross an arbitrary user hook while borrowing raw worker
// pointers from AppRuntimeGraph. The App mutex closes acquisition against graph
// reset; this gate lets existing borrowers release without needing that mutex.
// The run owner waits without the App mutex so a stop hook may still call
// workers()/workerFor(), then reacquires the mutex for the sole graph reset.
class AppRuntimeBorrowGate final {
public:
    void acquire() noexcept {
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    void release() noexcept {
        const auto previous = count_.fetch_sub(1, std::memory_order_acq_rel);
        if (previous == 0) {
            std::terminate();
        }
        if (previous == 1) {
            count_.notify_all();
        }
    }

    void wait() const noexcept {
        auto observed = count_.load(std::memory_order_acquire);
        while (observed != 0) {
            count_.wait(observed, std::memory_order_acquire);
            observed = count_.load(std::memory_order_acquire);
        }
    }

    [[nodiscard]] std::size_t count() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

private:
    mutable std::atomic_size_t count_{0};
};

struct AppDocumentRootConfig final {
    explicit AppDocumentRootConfig(std::pmr::memory_resource* resource)
        : root(resource),
          staticOptions(resource) {}

    NativePathString root;
    AppStaticRootOptions staticOptions;
    DocumentRootRuntimeOptions runtimeOptions;
};

struct AppListenerConfig final {
    AppListenerConfig(std::pmr::memory_resource* resource, std::string_view configuredAddress, std::uint16_t configuredPort, HttpServerOptions::PlainHttp)
        : address(configuredAddress, resource), port(configuredPort), transport(std::in_place_type<HttpServerOptions::PlainHttp>) {}

    AppListenerConfig(std::pmr::memory_resource* resource, std::string_view configuredAddress, std::uint16_t configuredPort, HttpServerOptions::Tls configuredTransport)
        : address(configuredAddress, resource), port(configuredPort), transport(std::in_place_type<HttpServerOptions::Tls>, std::move(configuredTransport)) {}

    AppListenerConfig(std::pmr::memory_resource* resource, std::string_view configuredAddress, std::uint16_t configuredPort, HttpServerOptions::RedirectHttpToHttps configuredTransport)
        : address(configuredAddress, resource), port(configuredPort), transport(std::in_place_type<HttpServerOptions::RedirectHttpToHttps>, configuredTransport) {}

    std::pmr::string address;
    std::uint16_t port;
    HttpServerOptions::ListenerTransport transport;
};

struct AppState final {
    AppState();
    ~AppState();

    std::pmr::vector<AppListenerConfig> listeners{appResource()};
    std::size_t workersPerListener;
    bool signalShutdown{false};
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
    Env env;
    std::unique_ptr<AppRuntimeGraph, PmrObjectDeleter<AppRuntimeGraph>> runtime;

    mutable std::mutex mutex;
    // App::stop() borrows the runtime graph across user stop hooks so it can
    // preserve the public hook-before-worker-close ordering without retaining
    // raw HttpServer pointers past graph destruction. App::run() is the sole
    // graph owner and waits for every such borrow before resetting it.
    AppRuntimeBorrowGate runtimeBorrows;
    AppLifecycle lifecycle;
};

}  // namespace ruvia::detail
