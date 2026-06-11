#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/app/Dotenv.h"
#include "ruvia/http/Controller.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/StaticFiles.h"
#include "ruvia/memory/MemoryPool.h"
#include "ruvia/router/Router.h"

#ifdef RUVIA_ENABLE_MARIADB
#include "ruvia/db/Db.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/redis/Redis.h"
#endif

namespace ruvia {

struct HttpServerOptions final {
    struct Tls final {
        bool enabled{false};
        std::filesystem::path certificateChainFile;
        std::filesystem::path privateKeyFile;
        std::pmr::string privateKeyPassword;
        std::filesystem::path verifyFile;
    };

    struct Compression final {
        bool enabled{true};
        std::size_t minBytes{1024};
    };

    struct Cors final {
        bool enabled{false};
        std::pmr::string allowOrigin{"*"};
        std::pmr::string allowHeaders;
        std::pmr::string exposeHeaders;
        std::chrono::seconds maxAge{std::chrono::seconds(0)};
        bool allowCredentials{false};
    };

    struct DocumentRoot final {
        const StaticRoot* root{nullptr};
    };

    std::chrono::milliseconds idleTimeout{std::chrono::seconds(60)};
    std::chrono::milliseconds scanInterval{std::chrono::seconds(1)};
    std::chrono::milliseconds headerTimeout{std::chrono::seconds(15)};
    std::chrono::milliseconds bodyTimeout{std::chrono::seconds(30)};
    std::chrono::milliseconds writeTimeout{std::chrono::seconds(30)};
    std::size_t maxConnections{0};
    std::size_t maxRequestsPerConnection{0};
    std::size_t maxBufferedBodyBytes{kDefaultMaxBufferedBodyBytes};
    std::size_t maxStreamBodyBytes{kDefaultMaxStreamBodyBytes};
    std::size_t maxWebSocketMessageBytes{kDefaultMaxWebSocketMessageBytes};
    Tls tls;
    Compression compression;
    Cors cors;
    DocumentRoot documentRoot;
};

struct TlsConfig final {
    std::filesystem::path certificateChainFile;
    std::filesystem::path privateKeyFile;
    std::pmr::string privateKeyPassword;
    std::filesystem::path verifyFile;
};

struct CompressionConfig final {
    bool enabled{true};
    std::size_t minBytes{1024};
};

struct CorsConfig final {
    bool enabled{false};
    std::pmr::string allowOrigin{"*"};
    std::pmr::string allowHeaders;
    std::pmr::string exposeHeaders;
    std::chrono::seconds maxAge{std::chrono::seconds(0)};
    bool allowCredentials{false};
};

struct DocumentRootConfig final {
    std::filesystem::path root;
    StaticRootOptions staticOptions;
};

namespace detail {

struct AppRuntimeGraph;
class HttpServer;

}  // namespace detail

class App final {
public:
    static App& instance();
    ~App();

    [[nodiscard]] const Env& env() const noexcept;
    App& loadDotenv(DotenvOptions options = {});
    App& loadDotenv(const std::filesystem::path& path, DotenvOptions options = {});
    App& setListenAddress(std::string_view address, std::uint16_t port);
    App& setThreadNum(std::size_t threadNum);
    App& setIdleTimeout(std::chrono::milliseconds timeout);
    App& setConnectionScanInterval(std::chrono::milliseconds interval);
    App& setHeaderTimeout(std::chrono::milliseconds timeout);
    App& setBodyTimeout(std::chrono::milliseconds timeout);
    App& setWriteTimeout(std::chrono::milliseconds timeout);
    App& setMaxConnectionsPerWorker(std::size_t maxConnections);
    App& setMaxRequestsPerConnection(std::size_t maxRequests);
    App& setMaxBufferedBodyBytes(std::size_t bytes);
    App& setMaxStreamBodyBytes(std::size_t bytes);
    App& setMaxWebSocketMessageBytes(std::size_t bytes);
    App& useTls(TlsConfig config);
    App& setCompression(CompressionConfig config);
    App& setCors(CorsConfig config);
    App& setDocumentRoot(DocumentRootConfig config);
    App& setDocumentRoot(const std::filesystem::path& root);
    App& setMemoryPoolConfig(MemoryPoolConfig config);
    App& setErrorHandler(HttpErrorHandler handler);
    template <typename MiddlewareT>
    App& use();
#ifdef RUVIA_ENABLE_MARIADB
    App& useDb(DbConfig config);
    App& useDb(std::string_view alias, DbConfig config);
#endif
#ifdef RUVIA_ENABLE_REDIS
    App& useRedis(RedisConfig config);
    App& useRedis(std::string_view alias, RedisConfig config);
#endif

    void run();
    void stop();

private:
    App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    std::pmr::string listenAddress_;
    std::uint16_t listenPort_;
    std::size_t threadNum_;
    HttpServerOptions options_{};
    std::optional<DocumentRootConfig> documentRootConfig_;
    MemoryPoolConfig memoryConfig_{};
    HttpErrorHandler errorHandler_{nullptr};
    std::pmr::vector<detail::ControllerMiddlewareDescriptor> globalMiddlewares_{
        ProcessMemory::instance().upstreamResource()};
#ifdef RUVIA_ENABLE_MARIADB
    std::pmr::vector<detail::DbDefinition> databases_{ProcessMemory::instance().upstreamResource()};
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::pmr::vector<detail::RedisDefinition> redis_{ProcessMemory::instance().upstreamResource()};
#endif

    Env env_;
    detail::ControllerStore controllerLifetimes_;
    std::unique_ptr<detail::AppRuntimeGraph> runtime_;
    Router router_;

    mutable std::mutex mutex_;
    bool autoControllersLoaded_{false};
    bool routeGraphFinalized_{false};
    bool running_{false};
};

template <typename MiddlewareT>
App& App::use() {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::logic_error("cannot register middleware while app is running");
    }
    if (routeGraphFinalized_) {
        throw std::logic_error("cannot register middleware after router finalize");
    }

    globalMiddlewares_.push_back(detail::makeMiddlewareDescriptor<MiddlewareT>());
    return *this;
}

App& app();

}  // namespace ruvia
