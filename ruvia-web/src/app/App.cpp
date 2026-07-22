#include "ruvia/web/detail/app/AppInternal.h"

#include <asio/signal_set.hpp>

#include <algorithm>
#include <csignal>
#include <exception>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/web/detail/controller/ControllerRuntime.h"
#include "ruvia/web/detail/app/AppConfigGuards.h"
#include "ruvia/core/detail/WorkerSelection.h"
#include "ruvia/web/detail/server/HttpServer.h"
#include "ruvia/web/detail/router/RouterInternal.h"

namespace ruvia {
namespace {

void addShutdownSignals(asio::signal_set& signals) {
    signals.add(SIGINT);
    signals.add(SIGTERM);
#if defined(SIGBREAK)
    signals.add(SIGBREAK);
#endif
}

[[nodiscard]] detail::HttpServerOptions makeListenerOptions(
    const detail::HttpServerOptions& base,
    detail::HttpServerOptions::ListenerTransport transport,
    const StaticRoot* documentRoot) {
    auto options = base;
    options.transport = std::move(transport);
    options.documentRoot.root = documentRoot;
    return options;
}

void assignTlsFileName(
    std::pmr::string& output,
    const std::filesystem::path& path) {
    const auto& native = path.native();
    output.assign(native.data(), native.size());
}

[[nodiscard]] detail::HttpServerOptions::Tls makeTlsOptions(
    const TlsConfig& config) {
    detail::HttpServerOptions::Tls tls;
    assignTlsFileName(
        tls.identity.certificateChainFile,
        config.identity().certificateChainFile());
    assignTlsFileName(
        tls.identity.privateKeyFile,
        config.identity().privateKeyFile());
    tls.identity.privateKeyPassword = config.identity().privateKeyPassword();
    if (config.clientCertificatePolicy().has_value()) {
        auto& policy = tls.clientCertificates.emplace(
            std::pmr::string{},
            config.clientCertificatePolicy()->requirement());
        assignTlsFileName(
            policy.verifyFile,
            config.clientCertificatePolicy()->verifyFile());
    }
    tls.sniIdentities.reserve(config.sniIdentities().size());
    for (const auto& configured : config.sniIdentities()) {
        auto& sni = tls.sniIdentities.emplace_back();
        sni.host = configured.host();
        assignTlsFileName(
            sni.identity.certificateChainFile,
            configured.identity().certificateChainFile());
        assignTlsFileName(
            sni.identity.privateKeyFile,
            configured.identity().privateKeyFile());
        sni.identity.privateKeyPassword = configured.identity().privateKeyPassword();
    }
    return tls;
}

void invokeStopHooks(detail::AppState& state) noexcept {
    for (auto& hook : state.onStopHooks) {
        try {
            hook();
        } catch (...) {
        }
    }
}

}  // namespace

namespace detail {

struct AppRuntimeGraph final {
    explicit AppRuntimeGraph(std::pmr::memory_resource* resource)
        : documentRoot(nullptr, PmrObjectDeleter<StaticRoot>{resource}),
          controllers(resource),
          routers(resource),
          workers(resource) {}

    std::unique_ptr<StaticRoot, PmrObjectDeleter<StaticRoot>> documentRoot;
    std::pmr::vector<ControllerStore> controllers;
    std::pmr::vector<std::unique_ptr<Router, PmrObjectDeleter<Router>>> routers;
    std::pmr::vector<std::unique_ptr<HttpServer, PmrObjectDeleter<HttpServer>>> workers;
};

AppState::AppState()
    : workersPerListener(std::max(1U, std::thread::hardware_concurrency())),
      runtime(nullptr, PmrObjectDeleter<AppRuntimeGraph>{detail::appResource()}) {
    listenAddress.assign("0.0.0.0");
}

AppState::~AppState() = default;

}  // namespace detail

namespace {

class AppRuntimeBorrow final {
public:
    AppRuntimeBorrow(
        detail::AppState& state,
        detail::AppRuntimeGraph* runtime) noexcept
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

void App::run() {
    auto& state = *state_;
    auto* runtimeResource = detail::appResource();
    const auto controllerRegistrars =
        detail::snapshotControllerRegistrars();
    std::pmr::vector<detail::HttpServer*> startedWorkers(runtimeResource);
    auto runtime = detail::makePmrObject<detail::AppRuntimeGraph>(runtimeResource, runtimeResource);

    {
        std::lock_guard lock(state.mutex);
        detail::ensureAppNotRunning(state.lifecycle.active(), "app is already running");

        auto preparedOptions = state.options;
        preparedOptions.env = &state.env;
        preparedOptions.workerFailure = detail::WorkerFailureSink{
            .target = this,
            .invoke = [](void* target, std::exception_ptr) noexcept {
                static_cast<App*>(target)->stop();
            },
        };

        if (state.documentRootConfig.has_value()) {
            const auto documentRootPath =
                std::filesystem::path(state.documentRootConfig->root.c_str());
            runtime->documentRoot = detail::makePmrObject<StaticRoot>(
                runtimeResource,
                documentRootPath,
                state.documentRootConfig->staticOptions);
        }

        const auto address = asio::ip::make_address(state.listenAddress);
        const auto hasTwoListeners = std::visit(
            []<typename Topology>(const Topology&) {
                return std::is_same_v<Topology, ServerTopology::HttpAndHttps> ||
                       std::is_same_v<Topology, ServerTopology::RedirectHttpToHttps>;
            },
            state.topology.topology_);
        const auto workerCount =
            state.workersPerListener * (hasTwoListeners ? 2 : 1);
        runtime->controllers.reserve(workerCount);
        runtime->routers.reserve(workerCount);
        runtime->workers.reserve(workerCount);

        const auto addWorkers = [&state, &address, &runtime,
                                 &controllerRegistrars,
                                 &preparedOptions, runtimeResource](
                                    std::uint16_t port,
                                    detail::HttpServerOptions::ListenerTransport transport) {
            const asio::ip::tcp::endpoint endpoint(address, port);
            auto listenerOptions = makeListenerOptions(
                preparedOptions,
                std::move(transport),
                runtime->documentRoot.get());
            for (std::size_t i = 0; i < state.workersPerListener; ++i) {
                auto workerOptions = i + 1 == state.workersPerListener
                    ? std::move(listenerOptions)
                    : listenerOptions;  // NOLINT(bugprone-use-after-move): moved only on the final iteration
                detail::ControllerStore controllers;
                auto router = detail::makePmrObject<Router>(runtimeResource);
                detail::registerControllers(
                    *router, controllers, controllerRegistrars);
                auto& routes = detail::RouterImpl::from(*router);
                routes.setErrorHandler(state.errorHandler);
                routes.setNotFoundHandler(state.notFoundHandler);
                if (!state.prefixErrorHandlers.empty()) {
                    std::pmr::vector<detail::HttpPrefixErrorHandler> views(
                        runtimeResource);
                    views.reserve(state.prefixErrorHandlers.size());
                    for (const auto& [prefix, handler] :
                         state.prefixErrorHandlers) {
                        views.push_back({std::string_view(prefix), handler});
                    }
                    routes.setPrefixErrorHandlers(views);
                }
                if (!state.prefixNotFoundHandlers.empty()) {
                    std::pmr::vector<detail::HttpPrefixNotFoundHandler> views(
                        runtimeResource);
                    views.reserve(state.prefixNotFoundHandlers.size());
                    for (const auto& [prefix, handler] :
                         state.prefixNotFoundHandlers) {
                        views.push_back({std::string_view(prefix), handler});
                    }
                    routes.setPrefixNotFoundHandlers(views);
                }
                if (!state.globalMiddlewares.empty()) {
                    routes.setGlobalMiddlewares(state.globalMiddlewares);
                }
                routes.finalize();
                auto worker = detail::makePmrObject<detail::HttpServer>(
                    runtimeResource,
                    endpoint,
                    routes.routeTable(),
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
                    state.workerStates,
                    std::move(workerOptions));
                runtime->controllers.push_back(std::move(controllers));
                runtime->routers.push_back(std::move(router));
                runtime->workers.push_back(std::move(worker));
            }
        };

        std::visit(
            [&]<typename Topology>(const Topology& topology) {
                if constexpr (std::is_same_v<Topology, ServerTopology::Http>) {
                    addWorkers(
                        topology.port,
                        detail::HttpServerOptions::PlainHttp{});
                } else if constexpr (
                    std::is_same_v<Topology, ServerTopology::Https>) {
                    addWorkers(
                        topology.port,
                        makeTlsOptions(topology.tls));
                } else if constexpr (
                    std::is_same_v<Topology, ServerTopology::HttpAndHttps>) {
                    addWorkers(
                        topology.httpPort,
                        detail::HttpServerOptions::PlainHttp{});
                    addWorkers(
                        topology.httpsPort,
                        makeTlsOptions(topology.tls));
                } else {
                    addWorkers(
                        topology.httpPort,
                        detail::HttpServerOptions::RedirectHttpToHttps{
                            topology.httpsPort});
                    addWorkers(
                        topology.httpsPort,
                        makeTlsOptions(topology.tls));
                }
            },
            state.topology.topology_);

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
            signalThread =
                std::thread([&signalContext] { signalContext.run(); });
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
                runDeferredStopHooks = state.lifecycle.completeStartHooks() ==
                    detail::AppStartHooksCompletion::kRunDeferredStopHooks;
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

    if (runStopHooks) {
        invokeStopHooks(state);
    }

    if (auto* borrowed = runtimeBorrow.get(); borrowed != nullptr) {
        for (const auto& worker : borrowed->workers) {
            worker->stop();
        }
    }
}

}  // namespace ruvia
