#include "ruvia/app/App.h"

#include <algorithm>
#include <memory_resource>
#include <stdexcept>
#include <thread>
#include <utility>

#include "ruvia/http/ControllerTypes.h"
#include "AppConfigGuards.h"
#include "../net/server/HttpServer.h"
#include "../router/RouterInternal.h"

namespace ruvia {
namespace {

void addShutdownSignals(asio::signal_set& signals) {
    signals.add(SIGINT);
    signals.add(SIGTERM);
#if defined(SIGBREAK)
    signals.add(SIGBREAK);
#endif
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

}  // namespace detail

App& App::instance() {
    static App instance;
    return instance;
}

App& app() {
    return App::instance();
}

App::App()
    : threadNum_(std::max(1U, std::thread::hardware_concurrency())) {
    listenAddress_.assign("0.0.0.0");
}

App::~App() = default;

const Env& App::env() const noexcept {
    return env_;
}

void App::run() {
    auto* runtimeResource = ProcessMemory::instance().upstreamResource();
    std::pmr::vector<detail::HttpServer*> startedWorkers(runtimeResource);
    auto runtime = detail::makePmrObject<detail::AppRuntimeGraph>(runtimeResource, runtimeResource);

    {
        std::lock_guard lock(mutex_);
        detail::ensureAppNotRunning(running_, "app is already running");

        if (!autoControllersLoaded_) {
            detail::registerControllers(router_, controllerLifetimes_);
            autoControllersLoaded_ = true;
        }
        auto& processMemory = ProcessMemory::instance();
        if (processMemory.frozen()) {
            if (processMemory.config().requestInitialBufferBytes != memoryConfig_.requestInitialBufferBytes) {
                throw std::logic_error("process memory configuration is already frozen with different values");
            }
        } else {
            processMemory.configure(memoryConfig_);
            processMemory.freeze();
        }
        auto& routes = detail::RouterImpl::from(router_);
        routes.setErrorHandler(errorHandler_);
        if (!routeGraphFinalized_) {
            routes.prependMiddlewares(globalMiddlewares_);
            routes.finalize();
            routeGraphFinalized_ = true;
        } else {
            routes.finalize();
        }
        const auto& routeTable = routes.routeTable();

        if (documentRootConfig_.has_value()) {
            runtime->documentRoot = detail::makePmrObject<StaticRoot>(
                runtimeResource,
                documentRootConfig_->root,
                documentRootConfig_->staticOptions);
        }

        if (httpsListenPort_.has_value() && !options_.tls.enabled) {
            throw std::invalid_argument("HTTPS listener requires TLS configuration");
        }
        if (!httpListenPort_.has_value() && !httpsListenPort_.has_value()) {
            throw std::invalid_argument("at least one HTTP or HTTPS listener must be configured");
        }
        if (autoHttps_) {
            if (!httpListenPort_.has_value() || !httpsListenPort_.has_value()) {
                throw std::invalid_argument("auto HTTPS requires both HTTP and HTTPS listeners");
            }
            if (*httpsListenPort_ == 0) {
                throw std::invalid_argument("auto HTTPS requires a fixed HTTPS listen port");
            }
        }
        if (httpListenPort_.has_value() && httpsListenPort_.has_value() &&
            *httpListenPort_ != 0 &&
            *httpListenPort_ == *httpsListenPort_) {
            throw std::invalid_argument("HTTP and HTTPS listen ports must be different");
        }

        const auto address = asio::ip::make_address(listenAddress_);
        const auto workerCount =
            (httpListenPort_.has_value() ? threadNum_ : 0) +
            (httpsListenPort_.has_value() ? threadNum_ : 0);
        runtime->workers.reserve(workerCount);

        const auto addWorkers = [&](std::uint16_t port, bool tlsEnabled, bool autoHttpsEnabled) {
            const asio::ip::tcp::endpoint endpoint(address, port);
            auto serverOptions = options_;
            serverOptions.tls.enabled = tlsEnabled;
            serverOptions.autoHttps.enabled = autoHttpsEnabled;
            serverOptions.autoHttps.httpsPort = httpsListenPort_.value_or(443);
            if (runtime->documentRoot != nullptr) {
                serverOptions.documentRoot.root = runtime->documentRoot.get();
            }
            for (std::size_t i = 0; i < threadNum_; ++i) {
                runtime->workers.push_back(detail::makePmrObject<detail::HttpServer>(
                    runtimeResource,
                    endpoint,
                    routeTable,
                    std::span<const detail::DbDefinition>{
#ifdef RUVIA_ENABLE_MARIADB
                        databases_
#endif
                    },
                    std::span<const detail::RedisDefinition>{
#ifdef RUVIA_ENABLE_REDIS
                        redis_
#endif
                    },
                    std::span<const detail::HttpClientDefinition>{
#ifdef RUVIA_ENABLE_HTTP_CLIENT
                        httpClients_
#endif
                    },
                    serverOptions));
            }
        };

        if (httpListenPort_.has_value()) {
            addWorkers(*httpListenPort_, false, autoHttps_);
        }
        if (httpsListenPort_.has_value()) {
            addWorkers(*httpsListenPort_, true, false);
        }

        runtime_ = std::move(runtime);
        running_ = true;
    }

    asio::io_context signalContext(1);
    asio::signal_set signals(signalContext);
    std::jthread signalThread;

    try {
        addShutdownSignals(signals);
        signals.async_wait([this](const std::error_code& ec, int) {
            if (!ec) {
                for (auto& hook : onStopHooks_) {
                    try { hook(); } catch (...) {}
                }
                stop();
            }
        });
        signalThread = std::jthread([&signalContext] { signalContext.run(); });

        for (const auto& worker : runtime_->workers) {
            worker->start();
            startedWorkers.push_back(worker.get());
        }

        for (auto& hook : onStartHooks_) {
            hook();
        }

        for (const auto& worker : runtime_->workers) {
            worker->join();
        }
    } catch (...) {
        for (auto* worker : startedWorkers) {
            worker->stop();
        }
        signals.cancel();
        signalContext.stop();

        std::lock_guard lock(mutex_);
        runtime_.reset();
        running_ = false;
        throw;
    }

    signals.cancel();
    signalContext.stop();

    std::lock_guard lock(mutex_);
    runtime_.reset();
    running_ = false;
}

void App::stop() {
    std::pmr::vector<detail::HttpServer*> workers(ProcessMemory::instance().upstreamResource());

    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }

        if (!runtime_) {
            return;
        }

        workers.reserve(runtime_->workers.size());
        for (const auto& worker : runtime_->workers) {
            workers.push_back(worker.get());
        }
    }

    for (auto* worker : workers) {
        worker->stop();
    }
}

}  // namespace ruvia
