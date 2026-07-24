#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/web/AppHook.h"
#include "ruvia/web/Dotenv.h"
#include "ruvia/web/RateLimitRule.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/ServerConfig.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/WebWorker.h"
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

class App final {
public:
    ~App();

    [[nodiscard]] const Env& env() const noexcept;
    App& loadDotenv(DotenvOptions options = {});
    App& loadDotenv(const std::filesystem::path& path, DotenvOptions options = {});
    App& setListenAddress(std::string_view address);
    App& setServerTopology(ServerTopology topology);
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

    // Worker-local user state, generalizing the per-worker db()/redis()
    // registries to application types: every worker builds its own T from the
    // registered factory at startup, and Context::workerState<T>() /
    // WebWorkerContext::workerState<T>() return that worker's instance.
    // Workers are single-threaded, so the instance needs no synchronization;
    // it must not be shared across workers by the application. One
    // registration per type; the factory and destructor run while that worker's
    // WorkerHandle reports isCurrent(), and a throwing factory fails run()
    // before any request is served.
    template <typename T, typename Factory>
    App& useWorkerState(Factory&& factory) {
        return useWorkerStateDefinition(
            detail::WorkerStateDefinition::make<T>(
                std::forward<Factory>(factory)));
    }

    // Enables Context::runBlocking(): one process-wide pool of ordinary threads
    // that handlers offload blocking work to, so a blocking call cannot freeze
    // the single-threaded worker that owns the connection. Absent by default --
    // an app that never offloads should not pay for idle threads, and one that
    // does should size the pool for its own workload. run() starts the threads
    // before the first request and joins them after the last worker.
    App& setBlockingPool(std::optional<BlockingPoolOptions> options);

    template <typename T>
    App& useWorkerState() {
        static_assert(
            std::is_default_constructible_v<T>,
            "useWorkerState<T>() without a factory requires T to be default "
            "constructible; pass a factory otherwise");
        return useWorkerState<T>([] { return T(); });
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
