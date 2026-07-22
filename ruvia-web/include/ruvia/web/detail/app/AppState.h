#pragma once

#include "ruvia/web/App.h"

#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/Router.h"
#include "ruvia/web/detail/app/AppLifecycle.h"
#include "ruvia/web/detail/app/AppResource.h"
#include "ruvia/web/detail/app/EnvState.h"
#include "ruvia/core/detail/util/NativePath.h"
#include "ruvia/web/detail/controller/ControllerDescriptors.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/DbTypes.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/RedisTypes.h"
#endif
#include "ruvia/web/detail/server/HttpServerOptions.h"

namespace ruvia::detail {

struct AppRuntimeGraph;

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
    bool signalShutdown{false};
    HttpServerOptions options{};
    std::optional<AppDocumentRootConfig> documentRootConfig;
    HttpErrorHandler errorHandler{nullptr};
    HttpNotFoundHandler notFoundHandler{nullptr};
    std::pmr::vector<std::pair<std::pmr::string, HttpErrorHandler>>
        prefixErrorHandlers{appResource()};
    std::pmr::vector<std::pair<std::pmr::string, HttpNotFoundHandler>>
        prefixNotFoundHandlers{appResource()};
    std::pmr::vector<ControllerMiddlewareDescriptor> globalMiddlewares{appResource()};
    std::pmr::vector<WorkerStateDefinition> workerStates{appResource()};
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
