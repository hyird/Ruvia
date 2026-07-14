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
#include "ruvia/core/detail/NativePath.h"
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
    bool tlsEnabled,
    bool autoHttpsEnabled,
    std::uint16_t httpsPort,
    const StaticRoot* documentRoot) {
    auto options = base;
    options.tls.enabled = tlsEnabled;
    options.autoHttps.enabled = autoHttpsEnabled;
    options.autoHttps.httpsPort = httpsPort;
    options.documentRoot.root = documentRoot;
    return options;
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
          workers(resource) {}

    std::unique_ptr<StaticRoot, PmrObjectDeleter<StaticRoot>> documentRoot;
    std::pmr::vector<std::unique_ptr<HttpServer, PmrObjectDeleter<HttpServer>>> workers;
};

AppState::AppState()
    : threadNum(std::max(1U, std::thread::hardware_concurrency())),
      runtime(nullptr, PmrObjectDeleter<AppRuntimeGraph>{detail::appResource()}) {
    listenAddress.assign("0.0.0.0");
}

AppState::~AppState() = default;

}  // namespace detail

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
    std::pmr::vector<detail::HttpServer*> startedWorkers(runtimeResource);
    auto runtime = detail::makePmrObject<detail::AppRuntimeGraph>(runtimeResource, runtimeResource);

    {
        std::lock_guard lock(state.mutex);
        detail::ensureAppNotRunning(state.running, "app is already running");

        if (!state.autoControllersLoaded) {
            detail::registerControllers(state.router, state.controllerLifetimes);
            state.autoControllersLoaded = true;
        }
        auto& processMemory = ProcessMemory::instance();
        if (processMemory.frozen()) {
            if (processMemory.config().requestInitialBufferBytes != state.memoryConfig.requestInitialBufferBytes) {
                throw std::logic_error("process memory configuration is already frozen with different values");
            }
        } else {
            processMemory.configure(state.memoryConfig);
            processMemory.freeze();
        }
        auto& routes = detail::RouterImpl::from(state.router);
        routes.setErrorHandler(state.errorHandler);
        routes.setNotFoundHandler(state.notFoundHandler);
        routes.finalize();
        const auto& routeTable = routes.routeTable();
        state.options.workerFailure = detail::WorkerFailureSink{
            .target = this,
            .invoke = [](void* target, std::exception_ptr) noexcept {
                static_cast<App*>(target)->stop();
            },
        };

        if (state.documentRootConfig.has_value()) {
            const auto documentRootPath = detail::makePathFromNativePath(state.documentRootConfig->root);
            runtime->documentRoot = detail::makePmrObject<StaticRoot>(
                runtimeResource,
                documentRootPath,
                state.documentRootConfig->staticOptions);
        }

        if (state.httpsListenPort.has_value() && !state.options.tls.enabled) {
            throw std::invalid_argument("HTTPS listener requires TLS configuration");
        }
        if (!state.httpListenPort.has_value() && !state.httpsListenPort.has_value()) {
            throw std::invalid_argument("at least one HTTP or HTTPS listener must be configured");
        }
        if (state.autoHttps) {
            if (!state.httpListenPort.has_value() || !state.httpsListenPort.has_value()) {
                throw std::invalid_argument("auto HTTPS requires both HTTP and HTTPS listeners");
            }
        }
        if (state.httpListenPort.has_value() && state.httpsListenPort.has_value() &&
            *state.httpListenPort == *state.httpsListenPort) {
            throw std::invalid_argument("HTTP and HTTPS listen ports must be different");
        }

        const auto address = asio::ip::make_address(state.listenAddress);
        const auto workerCount =
            (state.httpListenPort.has_value() ? state.threadNum : 0) +
            (state.httpsListenPort.has_value() ? state.threadNum : 0);
        runtime->workers.reserve(workerCount);

        const auto addWorkers = [&](std::uint16_t port, bool tlsEnabled, bool autoHttpsEnabled) {
            const asio::ip::tcp::endpoint endpoint(address, port);
            auto listenerOptions = makeListenerOptions(
                state.options,
                tlsEnabled,
                autoHttpsEnabled,
                state.httpsListenPort.value_or(443),
                runtime->documentRoot.get());
            for (std::size_t i = 0; i < state.threadNum; ++i) {
                auto workerOptions = i + 1 == state.threadNum
                    ? std::move(listenerOptions)
                    : listenerOptions;
                runtime->workers.push_back(detail::makePmrObject<detail::HttpServer>(
                    runtimeResource,
                    endpoint,
                    routeTable,
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
                    std::move(workerOptions)));
            }
        };

        if (state.httpListenPort.has_value()) {
            addWorkers(*state.httpListenPort, false, state.autoHttps);
        }
        if (state.httpsListenPort.has_value()) {
            addWorkers(*state.httpsListenPort, true, false);
        }

        state.runtime = std::move(runtime);
        state.stopRequested = false;
        state.startHooksRunning = false;
        state.stopHooksClaimed = false;
        state.running = true;
    }

    asio::io_context signalContext(1);
    asio::signal_set signals(signalContext);
    std::jthread signalThread;

    try {
        addShutdownSignals(signals);
        signals.async_wait([this](const std::error_code& ec, int) {
            if (!ec) {
                stop();
            }
        });
        signalThread = std::jthread([&signalContext] { signalContext.run(); });

        for (const auto& worker : state.runtime->workers) {
            {
                std::lock_guard lock(state.mutex);
                if (state.stopRequested) {
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
            shutdownRequestedDuringStartup = state.stopRequested;
        }
        if (shutdownRequestedDuringStartup) {
            for (auto* worker : startedWorkers) {
                worker->stop();
            }
        }

        bool runStartHooks = false;
        {
            std::lock_guard lock(state.mutex);
            if (!state.stopRequested) {
                state.startHooksRunning = true;
                runStartHooks = true;
            }
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
            state.startHooksRunning = false;
            if (state.stopRequested && !state.stopHooksClaimed) {
                state.stopHooksClaimed = true;
                runDeferredStopHooks = true;
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

        std::lock_guard lock(state.mutex);
        state.runtime.reset();
        state.running = false;
        throw;
    }

    signals.cancel();
    signalContext.stop();

    std::lock_guard lock(state.mutex);
    state.runtime.reset();
    state.running = false;
}
void App::stop() {
    auto& state = *state_;
    std::pmr::vector<detail::HttpServer*> workers(detail::appResource());
    bool runStopHooks = false;

    {
        std::lock_guard lock(state.mutex);
        if (!state.running || state.stopRequested) {
            return;
        }
        // Record the request durably so run()'s startup loop tears down any worker
        // it starts after this snapshot -- stop() below is a no-op on a worker that
        // is not started yet.
        state.stopRequested = true;
        if (!state.startHooksRunning && !state.stopHooksClaimed) {
            state.stopHooksClaimed = true;
            runStopHooks = true;
        }

        if (state.runtime) {
            workers.reserve(state.runtime->workers.size());
            for (const auto& worker : state.runtime->workers) {
                workers.push_back(worker.get());
            }
        }
    }

    if (runStopHooks) {
        invokeStopHooks(state);
    }

    for (auto* worker : workers) {
        worker->stop();
    }
}

}  // namespace ruvia
