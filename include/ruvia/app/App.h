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

#include "ruvia/app/AppHook.h"
#include "ruvia/app/Dotenv.h"
#include "ruvia/app/RateLimitRule.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/MiddlewareRuntime.h"
#include "ruvia/http/StaticFiles.h"
#include "ruvia/memory/MemoryPool.h"

#ifdef RUVIA_ENABLE_MARIADB
#include "ruvia/db/Db.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/redis/Redis.h"
#endif

#ifdef RUVIA_ENABLE_HTTP_CLIENT
#include "ruvia/http/HttpClient.h"
#endif

namespace ruvia {

// One completed request, passed to the access-log callback after the response is
// written. Views borrow request memory and are valid only for the call.
struct AccessLogRecord final {
    HttpMethod method{};
    std::string_view path;
    std::string_view remoteAddress;
    std::uint16_t status{0};
    std::uint64_t durationMicros{0};
    bool http2{false};
};

struct HttpServerOptions final {
    struct Tls final {
        // An additional certificate selected by SNI server name (RFC 6066).
        struct SniCertificate final {
            std::pmr::string host;
            std::pmr::string certificateChainFile;
            std::pmr::string privateKeyFile;
            std::pmr::string privateKeyPassword;
        };

        bool enabled{false};
        std::pmr::string certificateChainFile;
        std::pmr::string privateKeyFile;
        std::pmr::string privateKeyPassword;
        std::pmr::string verifyFile;
        std::pmr::vector<SniCertificate> sniCertificates;
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

    struct AutoHttps final {
        bool enabled{false};
        std::uint16_t httpsPort{443};
    };

    // Per-request observability hook. A raw function pointer plus user context
    // (no captures, no type erasure) so it is zero-cost on the request path when
    // unset and never allocates. Invoked once per completed request.
    struct AccessLog final {
        using Callback = void (*)(void* user, const AccessLogRecord& record) noexcept;
        Callback callback{nullptr};
        void* user{nullptr};
    };

    std::chrono::milliseconds idleTimeout{std::chrono::seconds(60)};
    // On stop, how long to let in-flight requests finish before force-closing
    // connections. 0 keeps the previous behavior (close immediately).
    std::chrono::milliseconds shutdownGracePeriod{0};
    // Scanner cadence; must be greater than 0.
    std::chrono::milliseconds scanInterval{std::chrono::seconds(1)};
    std::chrono::milliseconds headerTimeout{std::chrono::seconds(15)};
    std::chrono::milliseconds bodyTimeout{std::chrono::seconds(30)};
    std::chrono::milliseconds writeTimeout{std::chrono::seconds(30)};
    std::size_t maxConnections{0};
    std::size_t maxRequestsPerConnection{0};
    // Buffered routes materialize body data; this limit must be greater than 0.
    std::size_t maxBufferedBodyBytes{kDefaultMaxBufferedBodyBytes};
    // Stream routes are explicit; 0 disables the stream body limit.
    std::size_t maxStreamBodyBytes{kDefaultMaxStreamBodyBytes};
    // WebSocket messages are assembled before delivery; this limit must be greater than 0.
    std::size_t maxWebSocketMessageBytes{kDefaultMaxWebSocketMessageBytes};
    Tls tls;
    Compression compression;
    Cors cors;
    DocumentRoot documentRoot;
    AutoHttps autoHttps;
    AccessLog accessLog;
    RateLimitRule rateLimit;
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
    App& setListenAddress(std::string_view address, std::uint16_t port);
    App& setHttpListenPort(std::uint16_t port);
    App& setHttpsListenPort(std::uint16_t port);
    App& setAutoHttps(bool enabled = true);
    App& setThreadNum(std::size_t threadNum);
    App& setIdleTimeout(std::chrono::milliseconds timeout);
    App& setShutdownGracePeriod(std::chrono::milliseconds gracePeriod);
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
    App& addTlsCertificate(std::string_view host, TlsConfig config);
    App& setCompression(CompressionConfig config);
    App& setCors(CorsConfig config);
    App& setDocumentRoot(DocumentRootConfig config);
    App& setDocumentRoot(const std::filesystem::path& root);
    App& setMemoryPoolConfig(MemoryPoolConfig config);
    App& onError(HttpErrorHandler handler);
    App& setErrorHandler(HttpErrorHandler handler);
    App& notFound(HttpNotFoundHandler handler);
    App& setNotFoundHandler(HttpNotFoundHandler handler);
    App& setRateLimit(std::size_t maxRequests, std::chrono::milliseconds window = std::chrono::seconds(1));
    App& setRateLimit(RateLimitRule rule);
    App& setGlobalRateLimit(std::size_t maxRequests, std::chrono::milliseconds window = std::chrono::seconds(1));
    App& setGlobalRateLimit(RateLimitRule rule);
    App& onAccess(HttpServerOptions::AccessLog::Callback callback, void* user = nullptr);
    App& onStart(AppHook hook);
    App& onStop(AppHook hook);
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
#ifdef RUVIA_ENABLE_HTTP_CLIENT
    App& useHttpClient(HttpClientConfig config);
    App& useHttpClient(std::string_view alias, HttpClientConfig config);
#endif

    void run();
    void stop();

private:
    App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    App& useMiddleware(detail::ControllerMiddlewareDescriptor middleware);

    std::unique_ptr<detail::AppState, detail::AppStateDeleter> state_;
};

template <typename MiddlewareT>
App& App::use() {
    return useMiddleware(detail::makeMiddlewareDescriptor<MiddlewareT>());
}

App& app();

}  // namespace ruvia
