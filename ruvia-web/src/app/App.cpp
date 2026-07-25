#include "ruvia/web/detail/app/AppState.h"

#include "ruvia/core/detail/util/FailureReport.h"

#include <asio/signal_set.hpp>

#include <csignal>
#include <exception>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/detail/controller/ControllerRuntime.h"
#include "ruvia/web/detail/app/AppConfigGuards.h"
#include "ruvia/web/detail/app/AppListenerOptions.h"
#include "ruvia/core/detail/worker/WorkerSelection.h"
#include "ruvia/web/detail/server/HttpServer.h"
#include "ruvia/web/detail/router/RouterImpl.h"

namespace ruvia {
namespace {

[[nodiscard]] StaticRootOptions makeStaticRootOptions(const detail::AppStaticRootOptions& source) {
    StaticRootOptions result;
    result.cacheControl.assign(source.cacheControl);
    result.indexFile.assign(source.indexFile);
    result.defaultContentType.assign(source.defaultContentType);
    result.mimeTypes.reserve(source.mimeTypes.size());
    for (const auto& mimeType : source.mimeTypes) {
        result.mimeTypes.push_back(StaticMimeType{
            .extension = std::string(mimeType.extension),
            .contentType = std::string(mimeType.contentType),
        });
    }

    switch (source.fileTypeKind) {
        case StaticFileTypePolicy::Kind::kDefaults:
            result.fileTypes = StaticFileTypePolicy::defaults();
            break;
        case StaticFileTypePolicy::Kind::kAll:
            result.fileTypes = StaticFileTypePolicy::all();
            break;
        case StaticFileTypePolicy::Kind::kOnly: {
            std::vector<std::string_view> extensions;
            extensions.reserve(source.fileTypeExtensions.size());
            for (const auto& extension : source.fileTypeExtensions) {
                extensions.emplace_back(extension);
            }
            result.fileTypes = StaticFileTypePolicy::only(extensions);
            break;
        }
    }

    result.enableRanges = source.enableRanges;
    result.enableValidators = source.enableValidators;
    result.serveDotfiles = source.serveDotfiles;
    return result;
}

void addShutdownSignals(asio::signal_set& signals) {
    signals.add(SIGINT);
    signals.add(SIGTERM);
#if defined(SIGBREAK)
    signals.add(SIGBREAK);
#endif
}

void invokeStopHooks(detail::AppState& state) noexcept {
    for (auto& hook : state.onStopHooks) {
        try {
            hook();
        } catch (...) {
            // Shutdown must run every remaining hook, so one failure cannot
            // propagate -- but a hook that failed to release something is
            // exactly what an operator needs to see, and this runs after the
            // last caller that could have received it.
            detail::reportUnhandledFailure("app stop hook", std::current_exception());
        }
    }
}

}  // namespace

namespace detail {

struct AppRuntimeGraph final {
    explicit AppRuntimeGraph(std::pmr::memory_resource* resource)
        : documentRoot(nullptr, PmrObjectDeleter<StaticRoot>{resource}),
          blockingPool(nullptr, PmrObjectDeleter<BlockingPool>{resource}),
          controllers(resource),
          routers(resource),
          workers(resource) {}

    std::unique_ptr<StaticRoot, PmrObjectDeleter<StaticRoot>> documentRoot;
    // Declared before the workers so it is destroyed after them: a worker that
    // is still joining may hold a suspended handler whose task the pool owns.
    // Destroying the pool stops accepting work; its already-running callables
    // continue on the detached shared state until they return.
    std::unique_ptr<BlockingPool, PmrObjectDeleter<BlockingPool>> blockingPool;
    std::pmr::vector<ControllerStore> controllers;
    std::pmr::vector<std::unique_ptr<detail::Router, PmrObjectDeleter<detail::Router>>> routers;
    std::pmr::vector<std::unique_ptr<HttpServer, PmrObjectDeleter<HttpServer>>> workers;
};

AppState::AppState()
    : workersPerListener(std::max(1U, std::thread::hardware_concurrency())),
      runtime(nullptr, PmrObjectDeleter<AppRuntimeGraph>{detail::appResource()}) {
    listeners.emplace_back(detail::appResource(), "0.0.0.0", 8080, HttpServerOptions::PlainHttp{});
}

AppState::~AppState() = default;

}  // namespace detail

namespace {

class AppRuntimeBorrow final {
public:
    AppRuntimeBorrow(detail::AppState& state, detail::AppRuntimeGraph* runtime) noexcept
        : gate_(runtime == nullptr ? nullptr : &state.runtimeBorrows),
          runtime_(runtime) {}

    ~AppRuntimeBorrow() {
        if (gate_ != nullptr) {
            gate_->release();
        }
    }

    AppRuntimeBorrow(const AppRuntimeBorrow&) = delete;
    AppRuntimeBorrow& operator=(const AppRuntimeBorrow&) = delete;

    [[nodiscard]] detail::AppRuntimeGraph* get() const noexcept {
        return runtime_;
    }

private:
    detail::AppRuntimeBorrowGate* gate_;
    detail::AppRuntimeGraph* runtime_;
};

void completeAppRun(detail::AppState& state) noexcept {
    // The one successful stop request acquires its graph borrow in the same App
    // mutex critical section as the lifecycle transition to stopping. Once
    // worker joins finish no later stop call can acquire a new borrow. Do not
    // retain the mutex while waiting: the active user hook is allowed to inspect
    // workers()/workerFor() before it returns and releases its borrow.
    state.runtimeBorrows.wait();
    std::lock_guard lock(state.mutex);
    if (state.runtimeBorrows.count() != 0) {
        std::terminate();
    }
    state.runtime.reset();
    state.lifecycle.completeRun();
}

}  // namespace

App& app() {
    static App instance;
    return instance;
}

App::App()
    : state_(detail::constructPmrObject<detail::AppState>(detail::appResource())) {}

App::~App() = default;

void App::StateDeleter::operator()(detail::AppState* state) const noexcept {
    detail::destroyPmrObject(state, detail::appResource());
}

const Env& App::env() const noexcept {
    return state_->env;
}

HttpServerStats App::httpStats() const {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    HttpServerStats total;
    if (!state.runtime) {
        return total;
    }
    for (const auto& worker : state.runtime->workers) {
        const auto stats = worker->stats();
        total.activeConnections += stats.activeConnections;
        total.connectionsRefused += stats.connectionsRefused;
        total.connectionFailures += stats.connectionFailures;
        total.acceptFailures += stats.acceptFailures;
        total.workerFailures += stats.workerFailures;
        total.documentRootRefreshFailures += stats.documentRootRefreshFailures;
    }
    return total;
}

BlockingPoolStats App::blockingPoolStats() const {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    if (!state.runtime || !state.runtime->blockingPool) {
        return {};
    }
    return state.runtime->blockingPool->stats();
}

std::vector<WebWorkerHandle> App::workers() const {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    std::vector<WebWorkerHandle> result;
    if (!state.runtime) {
        return result;
    }
    result.reserve(state.runtime->workers.size());
    for (const auto& worker : state.runtime->workers) {
        result.push_back(worker->webWorker());
    }
    return result;
}

WebWorkerHandle App::workerFor(std::uint64_t key) const {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    if (!state.runtime || state.runtime->workers.empty()) {
        return {};
    }
    return state.runtime->workers[key % state.runtime->workers.size()]->webWorker();
}

WebWorkerHandle App::workerFor(std::string_view key) const {
    return workerFor(detail::workerSelectionHash(key));
}

namespace {

// One worker's router: the controllers registered at static-initialization time,
// then every handler the app was configured with -- error, not-found, their
// prefix-scoped variants, and the global middlewares -- and finalize. Each worker
// builds its own so a router is never shared across event loops.
[[nodiscard]] std::unique_ptr<detail::Router, detail::PmrObjectDeleter<detail::Router>> buildWorkerRouter(const detail::AppState& state, std::pmr::memory_resource* runtimeResource, detail::ControllerStore& controllers, std::span<const detail::ControllerRegistrar> controllerRegistrars) {
    auto router = detail::makePmrObject<detail::Router>(runtimeResource);
    detail::registerControllers(*router, controllers, controllerRegistrars);
    auto& routes = detail::RouterImpl::from(*router);
    routes.setErrorHandler(detail::CallbackAccess::ref(state.errorHandler));
    routes.setNotFoundHandler(detail::CallbackAccess::ref(state.notFoundHandler));
    if (!state.prefixErrorHandlers.empty()) {
        std::pmr::vector<detail::HttpPrefixErrorHandler> views(runtimeResource);
        views.reserve(state.prefixErrorHandlers.size());
        for (const auto& [prefix, handler] : state.prefixErrorHandlers) {
            views.push_back({std::string_view(prefix), detail::CallbackAccess::ref(handler)});
        }
        routes.setPrefixErrorHandlers(views);
    }
    if (!state.prefixNotFoundHandlers.empty()) {
        std::pmr::vector<detail::HttpPrefixNotFoundHandler> views(runtimeResource);
        views.reserve(state.prefixNotFoundHandlers.size());
        for (const auto& [prefix, handler] : state.prefixNotFoundHandlers) {
            views.push_back({std::string_view(prefix), detail::CallbackAccess::ref(handler)});
        }
        routes.setPrefixNotFoundHandlers(views);
    }
    if (!state.globalMiddlewares.empty()) {
        routes.setGlobalMiddlewares(state.globalMiddlewares);
    }
    routes.finalize();
    return router;
}

}  // namespace

void App::run() {
    auto& state = *state_;
    auto* runtimeResource = detail::appResource();
    const auto controllerRegistrars = detail::sealControllerRegistrars();
    std::pmr::vector<detail::HttpServer*> startedWorkers(runtimeResource);
    auto runtime = detail::makePmrObject<detail::AppRuntimeGraph>(runtimeResource, runtimeResource);

    {
        std::lock_guard lock(state.mutex);
        detail::ensureAppNotRunning(state.lifecycle.active(), "app is already running");

        auto preparedOptions = state.options;
        preparedOptions.env = &state.env;
        preparedOptions.workerFailure = detail::WorkerFailureSink{
            .target = this,
            .invoke = [](void* target, std::exception_ptr) noexcept { static_cast<App*>(target)->stop(); },
        };

        if (state.documentRootConfig.has_value()) {
            const auto documentRootPath = detail::makePathFromNativePath(state.documentRootConfig->root);
            runtime->documentRoot = detail::makePmrObject<StaticRoot>(runtimeResource, documentRootPath, makeStaticRootOptions(state.documentRootConfig->staticOptions));
            preparedOptions.documentRoot.runtimeOptions = state.documentRootConfig->runtimeOptions;
        }

        if (state.blockingPool.has_value()) {
            // Starting the threads here keeps them inside the same fallible
            // startup section as everything else: a pool that cannot start its
            // threads fails run() before a single connection is accepted.
            runtime->blockingPool = detail::makePmrObject<BlockingPool>(runtimeResource, *state.blockingPool);
            preparedOptions.blockingPool = runtime->blockingPool.get();
        }

        const auto workerCount = state.workersPerListener * state.listeners.size();
        runtime->controllers.reserve(workerCount);
        runtime->routers.reserve(workerCount);
        runtime->workers.reserve(workerCount);

        const auto addWorkers = [&state, &runtime, &controllerRegistrars, &preparedOptions, runtimeResource](const detail::AppListenerConfig& listener) {
            const asio::ip::tcp::endpoint endpoint(asio::ip::make_address(std::string_view(listener.address)), listener.port);
            auto listenerOptions = detail::makeListenerOptions(preparedOptions, listener.transport, runtime->documentRoot.get());
            for (std::size_t i = 0; i < state.workersPerListener; ++i) {
                auto workerOptions = i + 1 == state.workersPerListener ? std::move(listenerOptions) : listenerOptions;  // NOLINT(bugprone-use-after-move): moved only on
                                                                                                                        // the final iteration
                detail::ControllerStore controllers;
                auto router = buildWorkerRouter(state, runtimeResource, controllers, controllerRegistrars);
                auto& routes = detail::RouterImpl::from(*router);
                auto worker = detail::makePmrObject<detail::HttpServer>(runtimeResource, endpoint, routes.routeTable(),
                    std::span<const detail::DbDefinition>{
#ifdef RUVIA_ENABLE_DATABASE
                        state.databases
#endif
                    },
                    std::span<const detail::RedisDefinition>{
#ifdef RUVIA_ENABLE_REDIS
                        state.redis
#endif
                    },
                    state.workerStates, std::move(workerOptions));
                runtime->controllers.push_back(std::move(controllers));
                runtime->routers.push_back(std::move(router));
                runtime->workers.push_back(std::move(worker));
            }
        };

        for (const auto& listener : state.listeners) {
            addWorkers(listener);
        }

        // All fallible startup preparation is complete. Memory configuration is
        // already copied into each worker; no process-global state is committed.
        state.runtime = std::move(runtime);
        if (!state.lifecycle.beginRun()) {
            std::terminate();
        }
    }

    asio::io_context signalContext(1);
    asio::signal_set signals(signalContext);
    std::thread signalThread;

    try {
        if (state.signalShutdown) {
            addShutdownSignals(signals);
            signals.async_wait([this](const std::error_code& ec, int) {
                if (!ec) {
                    stop();
                }
            });
            signalThread = std::thread([&signalContext] { signalContext.run(); });
        }

        for (const auto& worker : state.runtime->workers) {
            {
                std::lock_guard lock(state.mutex);
                if (state.lifecycle.stopRequested()) {
                    break;
                }
            }
            worker->start();
            startedWorkers.push_back(worker.get());
        }

        // A shutdown (signal handler or a direct stop() call) can land while this
        // loop is still starting workers. stop() snapshots the full worker vector
        // and calls stop() on each, but stop() is a no-op on a worker this loop
        // has not started yet -- that worker would then start and run forever, so
        // the join() below would hang. Reconcile against a shutdown observed at any
        // point during startup by stopping everything we actually started; the
        // early break above only shrinks the window, it cannot close it, because
        // start() runs after the lock is released. HttpServer::stop() is idempotent.
        bool shutdownRequestedDuringStartup = false;
        {
            std::lock_guard lock(state.mutex);
            shutdownRequestedDuringStartup = state.lifecycle.stopRequested();
        }
        if (shutdownRequestedDuringStartup) {
            for (auto* worker : startedWorkers) {
                worker->stop();
            }
        }

        bool runStartHooks = false;
        {
            std::lock_guard lock(state.mutex);
            runStartHooks = state.lifecycle.beginStartHooks();
        }

        std::exception_ptr startHookFailure;
        if (runStartHooks) {
            try {
                for (auto& hook : state.onStartHooks) {
                    hook();
                }
            } catch (...) {
                startHookFailure = std::current_exception();
            }
        }

        bool runDeferredStopHooks = false;
        {
            std::lock_guard lock(state.mutex);
            if (runStartHooks) {
                runDeferredStopHooks = state.lifecycle.completeStartHooks() == detail::AppStartHooksCompletion::kRunDeferredStopHooks;
            }
        }
        if (runDeferredStopHooks) {
            invokeStopHooks(state);
        }
        if (startHookFailure) {
            std::rethrow_exception(startHookFailure);
        }

        for (const auto& worker : state.runtime->workers) {
            worker->join();
        }
    } catch (...) {
        stop();
        for (auto* worker : startedWorkers) {
            worker->stop();
        }
        signals.cancel();
        signalContext.stop();
        if (signalThread.joinable()) {
            signalThread.join();
        }

        completeAppRun(state);
        throw;
    }

    signals.cancel();
    signalContext.stop();
    if (signalThread.joinable()) {
        signalThread.join();
    }

    completeAppRun(state);
}
void App::stop() {
    auto& state = *state_;
    detail::AppRuntimeGraph* runtime = nullptr;
    bool runStopHooks = false;

    {
        std::lock_guard lock(state.mutex);
        const auto request = state.lifecycle.requestStop();
        if (request == detail::AppStopRequest::kIgnored) {
            return;
        }
        // Record the request durably so run()'s startup loop tears down any worker
        // it starts after this snapshot -- stop() below is a no-op on a worker that
        // is not started yet.
        runStopHooks = request == detail::AppStopRequest::kStopWorkersAndRunHooks;

        runtime = state.runtime.get();
        if (runtime != nullptr) {
            state.runtimeBorrows.acquire();
        }
    }
    AppRuntimeBorrow runtimeBorrow(state, runtime);

    if (auto* borrowed = runtimeBorrow.get(); borrowed != nullptr) {
        for (const auto& worker : borrowed->workers) {
            worker->stop();
        }
    }

    // Close every worker before application stop hooks run. WebWorkerDispatch::close()
    // requests the worker StopToken synchronously, which lets hooks join tasks that
    // are waiting in cancellable operations such as Redis XREADGROUP BLOCK 0.
    if (runStopHooks) {
        invokeStopHooks(state);
    }
}

}  // namespace ruvia
