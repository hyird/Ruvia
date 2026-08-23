#include "ruvia/web/detail/app/AppRunCoordinator.h"

#include <asio/signal_set.hpp>

#include <csignal>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/core/detail/util/FailureReport.h"
#include "ruvia/web/App.h"
#include "ruvia/web/detail/app/AppConfigGuards.h"
#include "ruvia/web/detail/app/AppRuntimeGraph.h"
#include "ruvia/web/detail/app/AppState.h"
#include "ruvia/web/detail/controller/ControllerRuntime.h"
#include "ruvia/web/detail/http/static/StaticRootIndex.h"
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
            result.fileTypes = {.kind = StaticFileTypePolicy::Kind::kDefaults};
            break;
        case StaticFileTypePolicy::Kind::kAll:
            result.fileTypes = {.kind = StaticFileTypePolicy::Kind::kAll};
            break;
        case StaticFileTypePolicy::Kind::kOnly: {
            std::vector<std::string> extensions;
            extensions.reserve(source.fileTypeExtensions.size());
            for (const auto& extension : source.fileTypeExtensions) {
                extensions.emplace_back(extension);
            }
            result.fileTypes = {
                .kind = StaticFileTypePolicy::Kind::kOnly,
                .extensions = std::move(extensions),
            };
            break;
        }
    }

    result.rangeRequests = source.rangeRequests;
    result.responseValidators = source.responseValidators;
    result.dotfiles = source.dotfiles;
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
            detail::reportUnhandledFailure("app stop hook", std::current_exception());
        }
    }
}

[[nodiscard]] std::unique_ptr<detail::Router, detail::PmrObjectDeleter<detail::Router>> buildWorkerRouter(
    const detail::AppState& state,
    std::pmr::memory_resource* runtimeResource,
    detail::ControllerStore& controllers,
    std::span<const detail::ControllerRegistrar> controllerRegistrars,
    const detail::CompiledRoutePlan* compiledPlan) {
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
    routes.finalize(compiledPlan);
    return router;
}

class AppRunCoordinator final {
public:
    AppRunCoordinator(App& owner, detail::AppState& state)
        : owner_(owner),
          state_(state),
          runtimeResource_(detail::appResource()),
          signalContext_(1),
          signals_(signalContext_) {}

    void run() {
        beginRun();

        try {
            buildAndPublishRuntime();
        } catch (...) {
            completeUnpublishedRun();
            throw;
        }

        std::exception_ptr primaryFailure;
        try {
            if (!stopRequested()) {
                startSignalHandling();
                startWorkers();
                runStartHooksAndWait();
            }
        } catch (...) {
            primaryFailure = std::current_exception();
            owner_.stop();
        }

        stopWorkers();
        stopSignalHandling();
        invokeStopHooks(state_);
        const auto workerFailure = joinWorkers();
        retireRuntime();

        if (primaryFailure != nullptr) {
            std::rethrow_exception(primaryFailure);
        }
        if (workerFailure != nullptr) {
            std::rethrow_exception(workerFailure);
        }
    }

private:
    void beginRun() {
        std::lock_guard lock(state_.mutex);
        detail::ensureAppNotRunning(state_.lifecycle.active(), "app is already running");
        if (!state_.lifecycle.beginRun()) {
            std::terminate();
        }
    }

    void buildAndPublishRuntime() {
        const auto controllerRegistrars = detail::sealControllerRegistrars();
        auto runtime = detail::makePmrObject<detail::AppRuntimeGraph>(runtimeResource_, runtimeResource_);
        auto preparedOptions = state_.options;
        preparedOptions.env = &state_.env;
        preparedOptions.workerFailure = detail::WorkerFailureSink{
            .target = &owner_,
            .invoke = [](void* target, std::exception_ptr) noexcept { static_cast<App*>(target)->stop(); },
        };

        std::unique_ptr<StaticRoot, detail::PmrObjectDeleter<StaticRoot>> configuredDocumentRoot(
            nullptr, detail::PmrObjectDeleter<StaticRoot>{runtimeResource_});
        if (state_.documentRootConfig.has_value()) {
            const auto documentRootPath = detail::makePathFromNativePath(state_.documentRootConfig->root);
            configuredDocumentRoot = detail::makePmrObject<StaticRoot>(runtimeResource_, documentRootPath, makeStaticRootOptions(state_.documentRootConfig->staticOptions));
            if (preparedOptions.compression.has_value() && state_.documentRootConfig->precompression.enabled()) {
                detail::StaticRootAccess::installPrecompressedVariants(
                    *configuredDocumentRoot, nullptr, state_.documentRootConfig->precompression);
            }
            preparedOptions.documentRoot = detail::HttpServerOptions::DocumentRoot::refreshing(
                *configuredDocumentRoot,
                state_.documentRootConfig->runtime,
                state_.documentRootConfig->precompression);
        }
        if (state_.blockingPool.has_value()) {
            runtime->blockingPool = detail::makePmrObject<BlockingPool>(runtimeResource_, *state_.blockingPool);
            preparedOptions.blockingPool = runtime->blockingPool.get();
        }

        std::pmr::vector<detail::HttpServerListenerDefinition> listeners(runtimeResource_);
        listeners.reserve(state_.listeners.size());
        for (const auto& listener : state_.listeners) {
            listeners.emplace_back(
                asio::ip::tcp::endpoint(asio::ip::make_address(std::string_view(listener.address)), listener.port),
                listener.transport);
        }

        runtime->workers.reserve(state_.workerCount);
        for (std::size_t i = 0; i < state_.workerCount; ++i) {
            auto workerOptions = i + 1 == state_.workerCount ? std::move(preparedOptions) : preparedOptions;
            detail::ControllerStore controllers;
            auto router = buildWorkerRouter(state_, runtimeResource_, controllers, controllerRegistrars, runtime->routePlan.get());
            auto& routes = detail::RouterImpl::from(*router);
            if (runtime->routePlan == nullptr) {
                runtime->routePlan = routes.releaseCompiledPlan();
            }
            const detail::WorkerCapabilityDefinitions capabilities{
#ifdef RUVIA_ENABLE_DATABASE
                .databases = std::span<const detail::DbDefinition>(state_.databases),
#endif
#ifdef RUVIA_ENABLE_REDIS
                .redis = std::span<const detail::RedisDefinition>(state_.redis),
#endif
                .workerStates = std::span<const detail::WorkerStateDefinition>(state_.workerStates),
                .httpClients = std::span<const detail::HttpClientDefinition>(state_.httpClients),
            };
            auto worker = detail::makePmrObject<detail::WebWorkerRuntime>(
                runtimeResource_,
                std::span<const detail::HttpServerListenerDefinition>(listeners),
                routes.routeTable(),
                capabilities,
                std::move(workerOptions));
            worker->prepare();
            runtime->workers.emplace_back(std::move(controllers), std::move(router), std::move(worker));
        }

        std::lock_guard lock(state_.mutex);
        state_.runtime = std::move(runtime);
        if (!state_.lifecycle.publishRuntime() && !state_.lifecycle.stopRequested()) {
            std::terminate();
        }
    }

    void completeUnpublishedRun() noexcept {
        std::lock_guard lock(state_.mutex);
        if (state_.runtime != nullptr) {
            std::terminate();
        }
        state_.lifecycle.completeRun();
    }

    [[nodiscard]] bool stopRequested() const {
        std::lock_guard lock(state_.mutex);
        return state_.lifecycle.stopRequested();
    }

    void startSignalHandling() {
        if (state_.processSignalHandlers != ProcessSignalHandlerPolicy::kInstall) {
            return;
        }
        addShutdownSignals(signals_);
        signals_.async_wait([this](const std::error_code& ec, int) {
            if (!ec) {
                owner_.stop();
            }
        });
        signalThread_ = std::thread([this] { signalContext_.run(); });
    }

    void startWorkers() {
        for (auto& worker : state_.runtime->workers) {
            if (stopRequested()) {
                return;
            }
            worker.runtime->launch();
        }
        for (auto& worker : state_.runtime->workers) {
            if (stopRequested()) {
                return;
            }
            worker.runtime->waitUntilReady();
        }
        for (auto& worker : state_.runtime->workers) {
            if (stopRequested()) {
                return;
            }
            worker.runtime->requestServe();
        }
        for (auto& worker : state_.runtime->workers) {
            if (stopRequested()) {
                return;
            }
            if (!worker.runtime->waitUntilServing()) {
                throw std::runtime_error("web worker stopped before the application began serving");
            }
        }
    }

    void runStartHooksAndWait() {
        if (stopRequested()) {
            return;
        }
        for (auto& hook : state_.onStartHooks) {
            hook();
        }

        std::unique_lock lock(state_.mutex);
        if (!state_.lifecycle.markRunning()) {
            return;
        }
        state_.lifecycleChanged.wait(lock, [this] { return state_.lifecycle.stopRequested(); });
    }

    void stopWorkers() noexcept {
        for (auto& worker : state_.runtime->workers) {
            try {
                worker.runtime->stop();
            } catch (...) {
                detail::reportUnhandledFailure("web worker stop", std::current_exception());
            }
        }
    }

    void stopSignalHandling() noexcept {
        std::error_code ignored;
        signals_.cancel(ignored);
        signalContext_.stop();
        if (signalThread_.joinable()) {
            signalThread_.join();
        }
    }

    [[nodiscard]] std::exception_ptr joinWorkers() noexcept {
        std::exception_ptr firstFailure;
        for (auto& worker : state_.runtime->workers) {
            try {
                worker.runtime->join();
            } catch (...) {
                if (firstFailure == nullptr) {
                    firstFailure = std::current_exception();
                } else {
                    detail::reportUnhandledFailure("additional web worker failure", std::current_exception());
                }
            }
        }
        return firstFailure;
    }

    void retireRuntime() noexcept {
        std::unique_ptr<detail::AppRuntimeGraph, detail::PmrObjectDeleter<detail::AppRuntimeGraph>> retired(
            nullptr, detail::PmrObjectDeleter<detail::AppRuntimeGraph>{runtimeResource_});
        {
            std::lock_guard lock(state_.mutex);
            retired = std::move(state_.runtime);
        }
        retired.reset();
        {
            std::lock_guard lock(state_.mutex);
            state_.lifecycle.completeRun();
        }
    }

    App& owner_;
    detail::AppState& state_;
    std::pmr::memory_resource* runtimeResource_;
    asio::io_context signalContext_;
    asio::signal_set signals_;
    std::thread signalThread_;
};

}  // namespace

void detail::runApp(App& app, AppState& state) {
    AppRunCoordinator(app, state).run();
}

}  // namespace ruvia
