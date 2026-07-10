#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/AppHook.h"
#include "ruvia/web/Dotenv.h"
#include "ruvia/web/RateLimitRule.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/HttpServerOptions.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/core/memory/MemoryPool.h"

#ifdef RUVIA_ENABLE_MARIADB
#include "ruvia/web/db/Db.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/Redis.h"
#endif

namespace ruvia {


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

struct AppState;
struct AppStateDeleter final {
    void operator()(AppState* state) const noexcept;
};

}  // namespace detail

class App final {
public:
    static App& instance();
    ~App();

    [[nodiscard]] const Env& env() const noexcept;
    App& loadDotenv(DotenvOptions options = {});
    App& loadDotenv(const std::filesystem::path& path, DotenvOptions options = {});
    App& setListenAddress(std::string_view address);
    App& setHttpListenPort(std::uint16_t port);
    App& setHttpsListenPort(std::uint16_t port);
    App& setAutoHttps(bool enabled = true);
    App& setThreadNum(std::size_t threadNum);
    App& setKeepaliveTimeout(std::chrono::milliseconds timeout);
    App& setShutdownGracePeriod(std::chrono::milliseconds gracePeriod);
    App& setConnectionScanInterval(std::chrono::milliseconds interval);
    App& setClientHeaderTimeout(std::chrono::milliseconds timeout);
    App& setClientBodyTimeout(std::chrono::milliseconds timeout);
    App& setSendTimeout(std::chrono::milliseconds timeout);
    App& setMaxConnectionsPerWorker(std::size_t maxConnections);
    App& setKeepaliveRequests(std::size_t maxRequests);
    App& setMaxBufferedBodyBytes(std::size_t bytes);
    App& setMaxStreamBodyBytes(std::size_t bytes);
    App& setMaxWebSocketMessageBytes(std::size_t bytes);
    App& useTls(TlsConfig config);
    App& addTlsCertificate(std::string_view host, TlsConfig config);
    App& setCompression(CompressionConfig config);
    App& setCors(CorsConfig config);
    App& setDocumentRoot(DocumentRootConfig config);
    App& setMemoryPoolConfig(MemoryPoolConfig config);
    App& onError(HttpErrorHandler handler);
    App& notFound(HttpNotFoundHandler handler);
    App& setGlobalRateLimit(RateLimitRule rule);
    App& onAccess(HttpServerOptions::AccessLog::Callback callback, void* user = nullptr);
    App& onStart(AppHook hook);
    App& onStop(AppHook hook);
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

    std::unique_ptr<detail::AppState, detail::AppStateDeleter> state_;
};

App& app();

}  // namespace ruvia
