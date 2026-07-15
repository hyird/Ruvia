#include "ruvia/web/detail/app/AppInternal.h"

#include <asio/signal_set.hpp>

#include <algorithm>
#include <csignal>
#include <exception>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
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
    detail::HttpServerOptions::ListenerTransport transport,
    const StaticRoot* documentRoot) {
    auto options = base;
    options.transport = std::move(transport);
    options.documentRoot.root = documentRoot;
    return options;
}

template <typename NativeChar>
void assignTlsFileNameFromNative(
    std::pmr::string& output,
    std::basic_string_view<NativeChar> native) {
    if constexpr (std::is_same_v<NativeChar, char>) {
        output.assign(native.data(), native.size());
    } else {
        const auto name = std::filesystem::path(native.begin(), native.end()).string();
        output.assign(name.data(), name.size());
    }
}

void assignTlsFileName(
    std::pmr::string& output,
    const std::filesystem::path& path) {
    assignTlsFileNameFromNative(output, detail::nativePathView(path));
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
        auto& policy = tls.clientCertificates.emplace();
        assignTlsFileName(
            policy.verifyFile,
            config.clientCertificatePolicy()->verifyFile());
        policy.requirement = config.clientCertificatePolicy()->requirement();
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
        detail::ensureAppNotRunning(state.lifecycle.active(), "app is already running");

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

        const auto address = asio::ip::make_address(state.listenAddress);
        const auto hasTwoListeners =
            state.topology.kind_ == ServerTopology::Kind::kHttpAndHttps ||
            state.topology.kind_ == ServerTopology::Kind::kRedirectHttpToHttps;
        const auto workerCount = state.threadNum * (hasTwoListeners ? 2 : 1);
        runtime->workers.reserve(workerCount);

        const auto addWorkers = [&state, &address, &runtime, &routeTable, runtimeResource](
                                    std::uint16_t port,
                                    detail::HttpServerOptions::ListenerTransport transport) {
            const asio::ip::tcp::endpoint endpoint(address, port);
            auto listenerOptions = makeListenerOptions(
                state.options,
                std::move(transport),
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

        switch (state.topology.kind_) {
            case ServerTopology::Kind::kHttp:
                addWorkers(
                    state.topology.httpPort_,
                    detail::HttpServerOptions::PlainHttp{});
                break;
            case ServerTopology::Kind::kHttps:
                addWorkers(
                    state.topology.httpsPort_,
                    makeTlsOptions(*state.topology.tls_));
                break;
            case ServerTopology::Kind::kHttpAndHttps:
                addWorkers(
                    state.topology.httpPort_,
                    detail::HttpServerOptions::PlainHttp{});
                addWorkers(
                    state.topology.httpsPort_,
                    makeTlsOptions(*state.topology.tls_));
                break;
            case ServerTopology::Kind::kRedirectHttpToHttps:
                addWorkers(
                    state.topology.httpPort_,
                    detail::HttpServerOptions::RedirectHttpToHttps{
                        state.topology.httpsPort_});
                addWorkers(
                    state.topology.httpsPort_,
                    makeTlsOptions(*state.topology.tls_));
                break;
        }

        state.runtime = std::move(runtime);
        if (!state.lifecycle.beginRun()) {
            throw std::logic_error("app lifecycle failed to begin run");
        }
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

        std::lock_guard lock(state.mutex);
        state.runtime.reset();
        state.lifecycle.completeRun();
        throw;
    }

    signals.cancel();
    signalContext.stop();

    std::lock_guard lock(state.mutex);
    state.runtime.reset();
    state.lifecycle.completeRun();
}
void App::stop() {
    auto& state = *state_;
    std::pmr::vector<detail::HttpServer*> workers(detail::appResource());
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
