#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/web/AppHook.h"
#include "ruvia/web/Dotenv.h"
#include "ruvia/web/RateLimitRule.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/HttpClientHandle.h"
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

#ifdef RUVIA_ENABLE_DATABASE
struct DbRegistrationConfig final {
    std::string alias{"default"};
    DbConfig config;
};
#endif

#ifdef RUVIA_ENABLE_REDIS
struct RedisRegistrationConfig final {
    std::string alias{"default"};
    RedisConfig config;
};
#endif

struct HttpClientRegistrationConfig final {
    std::string alias{"default"};
    HttpClientConfig config;
};

class App final : public detail::AppConfiguration<App> {
public:
    [[nodiscard]] const Env& env() const noexcept;
    App& loadDotenv(DotenvOptions options = {});
    App& loadDotenv(const std::filesystem::path& path, DotenvOptions options = {});
    App& server(ServerConfig config);
    App& listen(ListenConfig config);
    // The deployment's handler deadline. Absent by
    // default: an app that declares no deadline anywhere behaves exactly as
    // before and arms nothing per request. A route may tighten the handler
    // deadline with ruvia::Deadline<N> but never extend it -- the same rule
    // ServerConfig::maxBufferedBodyBytes follows.
    App& deadline(DeadlineConfig config);
    App& deadline(std::nullptr_t);
    // Disabled by default. A config enables response coding and precompressed
    // static variant negotiation; nullptr disables both.
    App& compression(CompressionConfig config);
    App& compression(std::nullptr_t);
    App& cors(CorsConfig config);
    App& cors(std::nullptr_t);
    App& documentRoot(DocumentRootConfig config);
    App& documentRoot(std::nullptr_t);
    // Configures the process-wide ordinary-thread pool used by
    // Context::runBlocking() and large buffered-response compression. It is
    // enabled by default with bounded CPU-based sizing; nullptr explicitly
    // disables it, making large buffered-response compression synchronous.
    // run() starts the threads before the first request; after workers stop,
    // running callables are not awaited and may finish on the pool's detached
    // state.
    App& blockingPool(BlockingPoolOptions config);
    App& blockingPool(std::nullptr_t);

    App& onError(HttpErrorHandler handler);
    App& onNotFound(HttpNotFoundHandler handler);
    // Path-prefix-scoped fallbacks, the Hono sub-app scoping analog: the
    // longest matching registered prefix wins, matching on whole path
    // segments ("/api" scopes "/api" and "/api/x", never "/apix"); the
    // prefix-less onError/notFound remain the app-wide fallback. A trailing
    // slash is ignored; registering the same normalized prefix twice throws
    // std::invalid_argument instead of silently choosing by call order.
    App& onError(ScopedErrorHandlerOptions options);
    App& onNotFound(ScopedNotFoundHandlerOptions options);
    // Peers whose forwarding headers name the real client. Accepts addresses
    // and CIDR blocks ("10.0.0.0/8", "2001:db8::/32", "127.0.0.1"); a malformed
    // entry throws std::invalid_argument at configuration time rather than
    // silently trusting nothing.
    //
    // Empty by default, and that default is the safe one: X-Forwarded-For is
    // client-controlled, so believing it from an arbitrary peer would let any
    // caller pick its own rate-limit key and claim a secure scheme. Configure
    // this ONLY with the addresses of proxies you operate, and the request's
    // ConnInfo::client()/scheme() then reflect the original caller.
    App& trustedProxies(TrustedProxyConfig config);
    App& trustedProxies(std::nullptr_t);

    // The rate limit every request passes. A route may add its own with
    // ruvia::RateLimit<max, windowMs>; both then apply, so the stricter is what
    // a caller actually meets -- the same "narrower scope may only tighten"
    // rule ServerConfig::maxBufferedBodyBytes follows.
    //
    // Worker-local: each worker counts independently, so a deployment with N
    // workers admits up to N times this rule. Size it accordingly.
    App& rateLimit(RateLimitConfig config);
    App& rateLimit(std::nullptr_t);
    App& onAccess(AccessLogCallback callback);
    // Observes connections lost to an exception that escaped their session --
    // the failures onError cannot answer because the response is already
    // committed or the error handler itself failed. Without a listener these
    // are reported to stderr; they are never silently dropped.
    App& onConnectionFailure(ConnectionFailureCallback callback);
    App& onStart(AppHook hook);
    App& onStop(AppHook hook);
#ifdef RUVIA_ENABLE_DATABASE
    App& database(DbRegistrationConfig config);
    App& database(std::nullptr_t);
#endif
#ifdef RUVIA_ENABLE_REDIS
    App& redis(RedisRegistrationConfig config);
    App& redis(std::nullptr_t);
#endif
    App& httpClient(HttpClientRegistrationConfig config);
    App& httpClient(std::nullptr_t);
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
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    std::unique_ptr<detail::AppState, StateDeleter> state_;
};

App& app();

}  // namespace ruvia
