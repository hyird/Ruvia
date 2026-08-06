#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/web/AppHook.h"
#include "ruvia/web/Dotenv.h"
#include "ruvia/web/RateLimitRule.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/ServerConfig.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/WebWorker.h"
#include "ruvia/web/detail/app/AppConfiguration.h"
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"
#include "ruvia/web/detail/integration/WorkerState.h"

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

class App final : public detail::AppConfiguration<App> {
public:
    ~App();

    [[nodiscard]] const Env& env() const noexcept;
    App& loadDotenv(DotenvOptions options = {});
    App& loadDotenv(const std::filesystem::path& path, DotenvOptions options = {});
    App& setListeners(std::vector<ListenerConfig> listeners);
    App& setWorkersPerListener(std::size_t workersPerListener);
    App& setSignalShutdown(bool enabled);
    App& setWorkerMailboxCapacity(std::size_t capacity);
    App& setKeepaliveTimeout(std::optional<std::chrono::milliseconds> timeout);
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
    // Enables Context::runBlocking(): one process-wide pool of ordinary threads
    // that handlers offload blocking work to, so a blocking call cannot freeze
    // the single-threaded worker that owns the connection. Absent by default --
    // an app that never offloads should not pay for idle threads, and one that
    // does should size the pool for its own workload. run() starts the threads
    // before the first request; after workers stop, running callables are not
    // awaited and may finish on the pool's detached state.
    App& setBlockingPool(std::optional<BlockingPoolOptions> options);

    App& onError(HttpErrorHandler handler);
    App& onNotFound(HttpNotFoundHandler handler);
    // Path-prefix-scoped fallbacks, the Hono sub-app scoping analog: the
    // longest matching registered prefix wins, matching on whole path
    // segments ("/api" scopes "/api" and "/api/x", never "/apix"); the
    // prefix-less onError/notFound remain the app-wide fallback. A trailing
    // slash is ignored; registering the same normalized prefix twice throws
    // std::invalid_argument instead of silently choosing by call order.
    App& onError(std::string_view prefix, HttpErrorHandler handler);
    App& onNotFound(std::string_view prefix, HttpNotFoundHandler handler);
    App& setDefaultRateLimitPerWorker(std::optional<RateLimitRule> rule);
    App& setRateLimitSlotsPerWorker(std::size_t slotsPerWorker);
    App& onAccess(AccessLogCallback callback);
    // Observes connections lost to an exception that escaped their session --
    // the failures onError cannot answer because the response is already
    // committed or the error handler itself failed. Without a listener these
    // are reported to stderr; they are never silently dropped.
    App& onConnectionFailure(ConnectionFailureCallback callback);
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
    // HTTP serving counters summed across every worker. All zero before run()
    // starts them and after it returns. Safe from any thread, including from a
    // stop hook.
    [[nodiscard]] HttpServerStats httpStats() const;
    // The blocking pool's counters, or all zero when no pool is configured or
    // the app is not running. Queue depth and the rejected count are what a
    // deployment sizes threadCount/queueCapacity from. Safe from any thread.
    [[nodiscard]] BlockingPoolStats blockingPoolStats() const;
    [[nodiscard]] std::vector<WebWorkerHandle> workers() const;
    [[nodiscard]] WebWorkerHandle workerFor(std::uint64_t key) const;
    [[nodiscard]] WebWorkerHandle workerFor(std::string_view key) const;

private:
    friend App& app();

    friend class detail::AppConfiguration<App>;

    App& useMiddleware(detail::ControllerMiddlewareDescriptor descriptor);
    App& useWorkerStateDefinition(detail::WorkerStateDefinition definition);

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
