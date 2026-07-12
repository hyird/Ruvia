#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

#include "ruvia/web/AppHook.h"
#include "ruvia/web/Dotenv.h"
#include "ruvia/web/RateLimitRule.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/ServerConfig.h"
#include "ruvia/core/memory/MemoryPool.h"

#ifdef RUVIA_ENABLE_MARIADB
#include "ruvia/web/db/Db.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/Redis.h"
#endif

namespace ruvia {


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
    App& setKeepaliveTimeout(std::optional<std::chrono::milliseconds> timeout);
    App& setShutdownGracePeriod(std::chrono::milliseconds gracePeriod);
    App& setConnectionScanInterval(std::chrono::milliseconds interval);
    App& setClientHeaderTimeout(std::optional<std::chrono::milliseconds> timeout);
    App& setClientBodyTimeout(std::optional<std::chrono::milliseconds> timeout);
    App& setSendTimeout(std::optional<std::chrono::milliseconds> timeout);
    App& setMaxConnectionsPerWorker(std::optional<std::size_t> maxConnections);
    App& setKeepaliveRequests(std::optional<std::size_t> maxRequests);
    App& setMaxBufferedBodyBytes(std::size_t bytes);
    App& setMaxStreamBodyBytes(std::optional<std::size_t> bytes);
    App& setMaxWebSocketMessageBytes(std::size_t bytes);
    App& useTls(TlsConfig config);
    App& addTlsCertificate(std::string_view host, TlsConfig config);
    App& setCompression(std::optional<CompressionConfig> config);
    App& setCors(std::optional<CorsConfig> config);
    App& setDocumentRoot(DocumentRootConfig config);
    App& setMemoryPoolConfig(MemoryPoolConfig config);
    App& onError(HttpErrorHandler handler);
    App& notFound(HttpNotFoundHandler handler);
    App& setGlobalRateLimit(std::optional<RateLimitRule> rule);
    App& onAccess(AccessLogCallback callback, void* user = nullptr);
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
