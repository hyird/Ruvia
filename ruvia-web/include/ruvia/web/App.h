#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "ruvia/web/AppHook.h"
#include "ruvia/web/Dotenv.h"
#include "ruvia/web/RateLimitRule.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/ServerConfig.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/WebWorker.h"
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/Db.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/Redis.h"
#endif

namespace ruvia {


namespace detail {

struct AppState;

}  // namespace detail

class App final {
public:
    ~App();

    [[nodiscard]] const Env& env() const noexcept;
    App& loadDotenv(DotenvOptions options = {});
    App& loadDotenv(const std::filesystem::path& path, DotenvOptions options = {});
    App& setListenAddress(std::string_view address);
    App& setServerTopology(ServerTopology topology);
    App& setWorkersPerListener(std::size_t workersPerListener);
    App& setWorkerMailboxCapacity(std::size_t capacity);
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
    App& setCompression(std::optional<CompressionConfig> config);
    App& setCors(std::optional<CorsConfig> config);
    App& setDocumentRoot(DocumentRootConfig config);
    App& setMemoryPoolConfig(MemoryPoolConfig config);
    // Hono app.use analog: registers one app-wide middleware instance that
    // runs before every matched route's controller and route middlewares, in
    // use() registration order. It participates only in routed dispatch;
    // requests that end in 404/405 without matching a route never enter a
    // middleware chain. Validator middlewares (RUVIA_VALIDATE_*) bind one
    // model to one route and are rejected here.
    template <typename MiddlewareT>
    App& use() {
        return useMiddleware(detail::makeMiddlewareDescriptor<MiddlewareT>());
    }

    App& onError(HttpErrorHandler handler);
    App& notFound(HttpNotFoundHandler handler);
    // Path-prefix-scoped fallbacks, the Hono sub-app scoping analog: the
    // longest matching registered prefix wins, matching on whole path
    // segments ("/api" scopes "/api" and "/api/x", never "/apix"); the
    // prefix-less onError/notFound remain the app-wide fallback. A trailing
    // slash is ignored and re-registering a prefix replaces its handler.
    App& onError(std::string_view prefix, HttpErrorHandler handler);
    App& notFound(std::string_view prefix, HttpNotFoundHandler handler);
    App& setDefaultRateLimitPerWorker(std::optional<RateLimitRule> rule);
    App& setRateLimitSlotsPerWorker(std::size_t slotsPerWorker);
    App& onAccess(AccessLogCallback callback);
    App& onStart(AppHook hook);
    App& onStop(AppHook hook);
#ifdef RUVIA_ENABLE_DATABASE
    App& useDb(DbConfig config);
    App& useDb(std::string_view alias, DbConfig config);
#endif
#ifdef RUVIA_ENABLE_REDIS
    App& useRedis(RedisConfig config);
    App& useRedis(std::string_view alias, RedisConfig config);
#endif
    void run();
    void stop();
    [[nodiscard]] std::vector<WebWorkerHandle> workers() const;
    [[nodiscard]] WebWorkerHandle workerFor(std::uint64_t key) const;
    [[nodiscard]] WebWorkerHandle workerFor(std::string_view key) const;

private:
    friend App& app();

    App& useMiddleware(detail::ControllerMiddlewareDescriptor descriptor);

    struct StateDeleter final {
        void operator()(detail::AppState* state) const noexcept;
    };

    App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    std::unique_ptr<detail::AppState, StateDeleter> state_;
};

App& app();

}  // namespace ruvia
