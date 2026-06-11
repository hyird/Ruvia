#include "ruvia/app/App.h"

#include "ruvia/http/Controller.h"
#include "../net/HttpServer.h"
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
    std::unique_ptr<StaticRoot> documentRoot;
    std::pmr::vector<std::unique_ptr<HttpServer>> workers{ProcessMemory::instance().upstreamResource()};
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
    : listenAddress_("0.0.0.0", ProcessMemory::instance().upstreamResource()),
      listenPort_(8080),
      threadNum_(std::max(1U, std::thread::hardware_concurrency())) {}

App::~App() = default;

const Env& App::env() const noexcept {
    return env_;
}

App& App::loadDotenv(DotenvOptions options) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot load dotenv while app is running");
    }

    (void)env_.loadFromExecutableDirectory(options);
    return *this;
}

App& App::loadDotenv(const std::filesystem::path& path, DotenvOptions options) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot load dotenv while app is running");
    }

    (void)env_.loadFromFile(path, options);
    return *this;
}

App& App::setListenAddress(std::string_view address, std::uint16_t port) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change listen address while app is running");
    }

    listenAddress_.assign(address.data(), address.size());
    listenPort_ = port;
    return *this;
}

App& App::setThreadNum(std::size_t threadNum) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change thread count while app is running");
    }
    if (threadNum == 0) {
        throw std::invalid_argument("thread count must be greater than 0");
    }

    threadNum_ = threadNum;
    return *this;
}

App& App::setIdleTimeout(std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change idle timeout while app is running");
    }
    if (timeout.count() < 0) {
        throw std::invalid_argument("idle timeout must not be negative");
    }

    options_.idleTimeout = timeout;
    return *this;
}

App& App::setConnectionScanInterval(std::chrono::milliseconds interval) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change connection scan interval while app is running");
    }
    if (interval.count() < 0) {
        throw std::invalid_argument("connection scan interval must not be negative");
    }

    options_.scanInterval = interval;
    return *this;
}

App& App::setHeaderTimeout(std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change header timeout while app is running");
    }
    if (timeout.count() < 0) {
        throw std::invalid_argument("header timeout must not be negative");
    }

    options_.headerTimeout = timeout;
    return *this;
}

App& App::setBodyTimeout(std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change body timeout while app is running");
    }
    if (timeout.count() < 0) {
        throw std::invalid_argument("body timeout must not be negative");
    }

    options_.bodyTimeout = timeout;
    return *this;
}

App& App::setWriteTimeout(std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change write timeout while app is running");
    }
    if (timeout.count() < 0) {
        throw std::invalid_argument("write timeout must not be negative");
    }

    options_.writeTimeout = timeout;
    return *this;
}

App& App::setMaxConnectionsPerWorker(std::size_t maxConnections) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change connection limit while app is running");
    }

    options_.maxConnections = maxConnections;
    return *this;
}

App& App::setMaxRequestsPerConnection(std::size_t maxRequests) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change keep-alive request limit while app is running");
    }

    options_.maxRequestsPerConnection = maxRequests;
    return *this;
}

App& App::setMaxBufferedBodyBytes(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change buffered body limit while app is running");
    }

    options_.maxBufferedBodyBytes = bytes;
    return *this;
}

App& App::setMaxStreamBodyBytes(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change stream body limit while app is running");
    }

    options_.maxStreamBodyBytes = bytes;
    return *this;
}

App& App::setMaxWebSocketMessageBytes(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change websocket message limit while app is running");
    }

    options_.maxWebSocketMessageBytes = bytes;
    return *this;
}

App& App::useTls(TlsConfig config) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot configure TLS while app is running");
    }
    if (config.certificateChainFile.empty()) {
        throw std::invalid_argument("TLS certificate chain file must not be empty");
    }
    if (config.privateKeyFile.empty()) {
        throw std::invalid_argument("TLS private key file must not be empty");
    }

    options_.tls.enabled = true;
    options_.tls.certificateChainFile = std::move(config.certificateChainFile);
    options_.tls.privateKeyFile = std::move(config.privateKeyFile);
    options_.tls.privateKeyPassword = std::move(config.privateKeyPassword);
    options_.tls.verifyFile = std::move(config.verifyFile);
    return *this;
}

App& App::setCompression(CompressionConfig config) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change compression config while app is running");
    }

    options_.compression.enabled = config.enabled;
    options_.compression.minBytes = config.minBytes;
    return *this;
}

App& App::setCors(CorsConfig config) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change CORS config while app is running");
    }
    if (config.enabled && config.allowOrigin.empty()) {
        throw std::invalid_argument("CORS allowOrigin must not be empty when CORS is enabled");
    }
    if (config.maxAge.count() < 0) {
        throw std::invalid_argument("CORS maxAge must not be negative");
    }

    options_.cors.enabled = config.enabled;
    options_.cors.allowOrigin = std::move(config.allowOrigin);
    options_.cors.allowHeaders = std::move(config.allowHeaders);
    options_.cors.exposeHeaders = std::move(config.exposeHeaders);
    options_.cors.maxAge = config.maxAge;
    options_.cors.allowCredentials = config.allowCredentials;
    return *this;
}

App& App::setDocumentRoot(DocumentRootConfig config) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change document root while app is running");
    }
    if (config.root.empty()) {
        throw std::invalid_argument("document root must not be empty");
    }
    if (config.staticOptions.indexFile.empty()) {
        config.staticOptions.indexFile = "index.html";
    }

    documentRootConfig_ = std::move(config);
    return *this;
}

App& App::setDocumentRoot(const std::filesystem::path& root) {
    DocumentRootConfig config;
    config.root = root;
    return setDocumentRoot(std::move(config));
}

App& App::setMemoryPoolConfig(MemoryPoolConfig config) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change memory pool config while app is running");
    }
    if (config.requestInitialBufferBytes == 0) {
        throw std::invalid_argument("memory pool config values must be greater than 0");
    }

    memoryConfig_ = config;
    return *this;
}

App& App::setErrorHandler(HttpErrorHandler handler) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot change error handler while app is running");
    }

    errorHandler_ = handler;
    return *this;
}

#ifdef RUVIA_ENABLE_MARIADB
App& App::useDb(DbConfig config) {
    return useDb("default", std::move(config));
}

App& App::useDb(std::string_view alias, DbConfig config) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot configure database while app is running");
    }
    if (alias.empty()) {
        throw std::invalid_argument("database alias must not be empty");
    }

    for (auto& definition : databases_) {
        if (std::string_view(definition.alias) == alias) {
            definition.config = std::move(config);
            return *this;
        }
    }

    std::pmr::string storedAlias(alias, ProcessMemory::instance().upstreamResource());
    databases_.push_back(detail::DbDefinition{std::move(storedAlias), std::move(config)});
    return *this;
}
#endif

#ifdef RUVIA_ENABLE_REDIS
App& App::useRedis(RedisConfig config) {
    return useRedis("default", std::move(config));
}

App& App::useRedis(std::string_view alias, RedisConfig config) {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot configure redis while app is running");
    }
    if (alias.empty()) {
        throw std::invalid_argument("redis alias must not be empty");
    }

    for (auto& definition : redis_) {
        if (std::string_view(definition.alias) == alias) {
            definition.config = std::move(config);
            return *this;
        }
    }

    std::pmr::string storedAlias(alias, ProcessMemory::instance().upstreamResource());
    redis_.push_back(detail::RedisDefinition{std::move(storedAlias), std::move(config)});
    return *this;
}
#endif

void App::run() {
    std::pmr::vector<detail::HttpServer*> startedWorkers(ProcessMemory::instance().upstreamResource());
    auto runtime = std::make_unique<detail::AppRuntimeGraph>();

    {
        std::lock_guard lock(mutex_);
        if (running_) {
            throw std::logic_error("app is already running");
        }

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

        const auto address = asio::ip::make_address(listenAddress_);
        const asio::ip::tcp::endpoint endpoint(address, listenPort_);
        auto serverOptions = options_;
        if (documentRootConfig_.has_value()) {
            runtime->documentRoot = std::make_unique<StaticRoot>(
                documentRootConfig_->root,
                documentRootConfig_->staticOptions);
            serverOptions.documentRoot.root = runtime->documentRoot.get();
        }

        runtime->workers.reserve(threadNum_);
        for (std::size_t i = 0; i < threadNum_; ++i) {
            runtime->workers.push_back(std::make_unique<detail::HttpServer>(
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
                serverOptions));
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
                stop();
            }
        });
        signalThread = std::jthread([&signalContext] { signalContext.run(); });

        for (const auto& worker : runtime_->workers) {
            worker->start();
            startedWorkers.push_back(worker.get());
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
