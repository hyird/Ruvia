#include "ruvia/web/detail/app/AppRunCoordinator.h"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/signal_set.hpp>

#include "ruvia/core/FailureReport.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/web/App.h"
#include "ruvia/web/detail/app/AppConfigGuards.h"
#include "ruvia/web/detail/app/AppRuntimeGraph.h"
#include "ruvia/web/detail/app/AppState.h"
#include "ruvia/web/detail/controller/ControllerRuntime.h"
#include "ruvia/web/detail/http/static/StaticRootIndex.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"

namespace ruvia {
namespace {

void addShutdownSignals(asio::signal_set& signals) {
    signals.add(SIGINT);
    signals.add(SIGTERM);
#if defined(SIGBREAK)
    signals.add(SIGBREAK);
#endif
}

void acceptor_failed(void* object) noexcept {
    static_cast<App*>(object)->stop();
}

bool acceptor_target_available(void* object) noexcept {
    return static_cast<detail::WebWorkerRuntime*>(object)->availableForNetworkDispatch();
}

void acceptor_target_accept(void* object, detail::NativeAcceptedSocketTicket&& ticket) noexcept {
    static_cast<detail::WebWorkerRuntime*>(object)->acceptTransferredConnection(std::move(ticket));
}

void stage_worker_quic(void* object, detail::http3_datagram_channel& channel,
    asio::ip::udp::endpoint endpoint, quic_cid_partition partition) {
    static_cast<detail::WebWorkerRuntime*>(object)->stage_quic(channel, endpoint, partition);
}

void invokeStopHooks(detail::AppState& state) noexcept {
    for (auto& hook : state.onStopHooks) {
        try {
            hook();
        } catch (...) {
            ruvia::reportUnhandledFailure("app stop hook", std::current_exception());
        }
    }
}

[[nodiscard]] std::unique_ptr<detail::Router, detail::PmrObjectDeleter<detail::Router>>
buildWorkerRouter(const detail::AppState& state, std::pmr::memory_resource* runtimeResource,
    detail::ControllerStore& controllers,
    std::span<const detail::ControllerRegistrar> controllerRegistrars,
    const detail::CompiledRoutePlan* compiledPlan) {
    auto router = detail::makePmrObject<detail::Router>(runtimeResource);
    detail::registerControllers(*router, controllers, controllerRegistrars);
    auto& routes = detail::RouterImpl::from(*router);
    state.configuration.apply(routes, runtimeResource);
    routes.finalize(compiledPlan);
    return router;
}

class AppRunCoordinator final {
public:
    AppRunCoordinator(App& owner, detail::AppState& state)
        : owner_(owner),
          state_(state),
          runtimeResource_(detail::appResource()),
          signal_runtime_({.queue_capacity = 128}),
          signals_(signal_runtime_.context().ioContext()) {
        signal_runtime_.configure({
            .stop_admission = [this] {
                std::error_code ignored;
                signals_.cancel(ignored);
                signal_runtime_.finalize(); },
            .failure = [this](std::exception_ptr) noexcept { owner_.stop(); },
        });
    }

    ~AppRunCoordinator() {
        stopSignalHandling();
    }

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
                run_start_hooks();
                start_serving();
                wait_for_stop();
            }
        } catch (...) {
            primaryFailure = std::current_exception();
            owner_.stop();
        }

        stopWorkers();
        stopSignalHandling();
        invokeStopHooks(state_);
        const auto workerFailure = joinWorkers();
        const auto signal_failure = signal_runtime_.failure();
        retireRuntime();

        if (primaryFailure != nullptr) {
            std::rethrow_exception(primaryFailure);
        }
        if (signal_failure != nullptr) {
            std::rethrow_exception(signal_failure);
        }
        if (workerFailure != nullptr) {
            std::rethrow_exception(workerFailure);
        }
    }

private:
    void beginRun() {
        std::lock_guard lock(state_.mutex);
        detail::ensureAppNotRunning(state_.lifecycle.active(), "app is already running");
        if (state_.listeners.empty()) {
            throw std::invalid_argument(
                "App::run() requires at least one listener; call App::listen() first");
        }
        if (!state_.lifecycle.beginRun()) {
            std::terminate();
        }
    }

    void buildAndPublishRuntime() {
        const auto controllerRegistrars = detail::sealControllerRegistrars();
        auto runtime =
            detail::makePmrObject<detail::AppRuntimeGraph>(runtimeResource_, runtimeResource_);
        auto preparedOptions = state_.options;
        preparedOptions.env = &state_.env;
        preparedOptions.workerFailure = detail::WorkerFailureSink{
            .target = &owner_,
            .invoke =
                [](void* target, const std::exception_ptr&) noexcept {
                    static_cast<App*>(target)->stop();
                },
        };

        std::unique_ptr<StaticRoot, detail::PmrObjectDeleter<StaticRoot>> configuredDocumentRoot(
            nullptr, detail::PmrObjectDeleter<StaticRoot>{runtimeResource_});
        if (state_.documentRootConfig.has_value()) {
            const auto documentRootPath =
                ruvia::makePathFromNativePath(state_.documentRootConfig->root);
            configuredDocumentRoot = detail::StaticRootAccess::make(
                runtimeResource_, documentRootPath, state_.documentRootConfig->staticOptions);
            if (preparedOptions.compression.has_value() &&
                state_.documentRootConfig->precompression.enabled()) {
                detail::StaticRootAccess::installPrecompressedVariants(
                    *configuredDocumentRoot, nullptr, state_.documentRootConfig->precompression);
            }
            preparedOptions.documentRoot =
                detail::HttpServerOptions::DocumentRoot::refreshing(*configuredDocumentRoot,
                    state_.documentRootConfig->runtime, state_.documentRootConfig->precompression);
        }
        if (state_.blockingPool.has_value()) {
            runtime->blockingPool =
                detail::makePmrObject<BlockingPool>(runtimeResource_, *state_.blockingPool);
            preparedOptions.blockingPool = runtime->blockingPool.get();
        }

        const auto validatedConfiguration =
            detail::validateHttpServerConfiguration(state_.listeners, std::move(preparedOptions));

        runtime->workers.reserve(state_.workerCount);
        for (std::size_t i = 0; i < state_.workerCount; ++i) {
            detail::ControllerStore controllers;
            auto router = buildWorkerRouter(state_, runtimeResource_, controllers,
                controllerRegistrars, runtime->routePlan.get());
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
                .workerStates = state_.configuration.worker_states(),
                .httpClients = std::span<const detail::HttpClientDefinition>(state_.httpClients),
            };
            auto worker = detail::makePmrObject<detail::WebWorkerRuntime>(
                runtimeResource_, validatedConfiguration, routes.routeTable(), capabilities);
            worker->prepare();
            runtime->workers.emplace_back(
                std::move(controllers), std::move(router), std::move(worker));
        }

        const bool hasHttp3 = std::ranges::any_of(validatedConfiguration.listeners(),
            [](const detail::HttpServerListenerDefinition& listener) {
                return listener.http3.has_value();
            });
        runtime->acceptor_targets.reserve(runtime->workers.size());
        for (auto& slot : runtime->workers) {
            auto* target = slot.runtime.get();
            runtime->acceptor_targets.push_back({
                .submission = target->networkSubmission(),
                .object = target,
                .available = &acceptor_target_available,
                .accept = &acceptor_target_accept,
                .stage_quic = hasHttp3 ? &stage_worker_quic : nullptr,
            });
        }
        runtime->ingress = std::make_unique<detail::acceptor>(
            validatedConfiguration.listeners(), runtime->acceptor_targets, &owner_, &acceptor_failed);
        runtime->ingress->prepare();

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
        signal_runtime_.start();
    }

    void startWorkers() {
        // Bind ingress and stage the datagram channels before workers construct
        // their own QUIC/TLS and HTTP/3 state.
        {
            std::lock_guard lock(state_.mutex);
            if (state_.lifecycle.stopRequested()) {
                return;
            }
            state_.runtime->ingress->launch();
        }
        state_.runtime->ingress->wait_until_ready();
        state_.runtime->ingress->rethrow_failure();
        if (stopRequested()) {
            return;
        }

        for (auto& worker : state_.runtime->workers) {
            {
                std::lock_guard lock(state_.mutex);
                if (state_.lifecycle.stopRequested()) {
                    return;
                }
                worker.runtime->launch();
            }
        }
        for (auto& worker : state_.runtime->workers) {
            if (stopRequested()) {
                return;
            }
            worker.runtime->waitUntilReady();
        }
    }

    void start_serving() {
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
                if (stopRequested()) {
                    return;
                }
                throw std::runtime_error("web worker stopped before the application began serving");
            }
        }
        if (!stopRequested()) {
            state_.runtime->ingress->request_serve();
            if (!state_.runtime->ingress->wait_until_serving()) {
                state_.runtime->ingress->rethrow_failure();
                if (stopRequested()) {
                    return;
                }
                throw std::runtime_error("acceptor stopped before the application began serving");
            }
        }
    }

    void run_start_hooks() {
        for (auto& hook : state_.onStartHooks) {
            if (stopRequested()) {
                return;
            }
            hook();
        }
    }

    void wait_for_stop() {
        std::unique_lock lock(state_.mutex);
        if (!state_.lifecycle.markRunning()) {
            return;
        }
        state_.lifecycleChanged.wait(lock, [this] { return state_.lifecycle.stopRequested(); });
    }

    void stopWorkers() noexcept {
        if (state_.runtime->ingress) {
            state_.runtime->ingress->stop();
        }
        for (auto& worker : state_.runtime->workers) {
            try {
                worker.runtime->stopAdmission();
            } catch (...) {
                ruvia::reportUnhandledFailure("web worker stop", std::current_exception());
            }
        }
    }

    void stopSignalHandling() noexcept {
        signal_runtime_.request_stop();
        signal_runtime_.join();
    }

    [[nodiscard]] std::exception_ptr joinWorkers() noexcept {
        std::exception_ptr firstFailure;
        // The acceptor joins after datagram publishers acknowledge closure.
        // Workers retain their own protocol state until their task scopes join.
        bool acceptor_quiesced = state_.runtime->ingress == nullptr;
        if (state_.runtime->ingress) {
            try {
                state_.runtime->ingress->join();
                acceptor_quiesced = true;
                state_.runtime->ingress->rethrow_failure();
            } catch (...) {
                if (!acceptor_quiesced) {
                    std::terminate();
                }
                firstFailure = std::current_exception();
            }
        }
        if (!acceptor_quiesced) {
            std::terminate();
        }
        for (auto& worker : state_.runtime->workers) {
            try {
                worker.runtime->finalizeAfterNetworkQuiesced();
                worker.runtime->join();
            } catch (...) {
                if (firstFailure == nullptr) {
                    firstFailure = std::current_exception();
                } else {
                    ruvia::reportUnhandledFailure(
                        "additional web worker failure", std::current_exception());
                }
            }
        }
        return firstFailure;
    }

    void retireRuntime() noexcept {
        std::unique_ptr<detail::AppRuntimeGraph, detail::PmrObjectDeleter<detail::AppRuntimeGraph>>
            retired(nullptr, detail::PmrObjectDeleter<detail::AppRuntimeGraph>{runtimeResource_});
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
    worker_runtime signal_runtime_;
    asio::signal_set signals_;
};

}  // namespace

void detail::runApp(App& app, AppState& state) {
    AppRunCoordinator(app, state).run();
}

}  // namespace ruvia
