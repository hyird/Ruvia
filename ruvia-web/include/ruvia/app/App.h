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
#include "ruvia/http/StaticFiles.h"
#include "ruvia/memory/MemoryPool.h"

#ifdef RUVIA_ENABLE_MARIADB
#include "ruvia/db/Db.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/redis/Redis.h"
#endif
#include "ruvia/http/HttpClient.h"

namespace ruvia {

namespace detail {
struct AccessLogRecordAccess;
}  // namespace detail

// One completed request, passed to the access-log callback after the response is
// written. Views borrow request memory and are valid only for the call.
class AccessLogRecord final {
public:
    [[nodiscard]] constexpr HttpMethod method() const noexcept {
        return method_;
    }

    [[nodiscard]] constexpr std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] constexpr std::string_view remoteAddress() const noexcept {
        return remoteAddress_;
    }

    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr std::uint64_t durationMicros() const noexcept {
        return durationMicros_;
    }

    [[nodiscard]] constexpr bool http2() const noexcept {
        return http2_;
    }

private:
    friend struct detail::AccessLogRecordAccess;

    constexpr AccessLogRecord(
        HttpMethod method,
        std::string_view path,
        std::string_view remoteAddress,
        std::uint16_t status,
        std::uint64_t durationMicros,
        bool http2) noexcept
        : method_(method),
          path_(path),
          remoteAddress_(remoteAddress),
          status_(status),
          durationMicros_(durationMicros),
          http2_(http2) {}

    HttpMethod method_;
    std::string_view path_;
    std::string_view remoteAddress_;
    std::uint16_t status_;
    std::uint64_t durationMicros_;
    bool http2_;
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

    // nginx-aligned timeouts (names + inactivity semantics + defaults). All are inactivity
    // timeouts: the timer resets on each successful read/write and fires only after a gap of the
    // given duration. Set any to 0 to disable it.
    //   keepaliveTimeout   == nginx keepalive_timeout   (idle keep-alive between requests)
    //   clientHeaderTimeout== nginx client_header_timeout(gap while reading the request head; also
    //                         bounds the TLS handshake)
    //   clientBodyTimeout  == nginx client_body_timeout (gap while reading the request body)
    //   sendTimeout        == nginx send_timeout        (gap while writing the response)
    //   keepaliveRequests  == nginx keepalive_requests  (max requests per kept-alive connection)
    std::chrono::milliseconds keepaliveTimeout{std::chrono::seconds(75)};
    // On stop, how long to let in-flight requests finish before force-closing
    // connections. 0 keeps the previous behavior (close immediately).
    std::chrono::milliseconds shutdownGracePeriod{0};
    // Scanner cadence; must be greater than 0.
    std::chrono::milliseconds scanInterval{std::chrono::seconds(1)};
    std::chrono::milliseconds clientHeaderTimeout{std::chrono::seconds(60)};
    std::chrono::milliseconds clientBodyTimeout{std::chrono::seconds(60)};
    std::chrono::milliseconds sendTimeout{std::chrono::seconds(60)};
    std::size_t maxConnections{0};
    std::size_t keepaliveRequests{1000};
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
    // Register an upstream HTTP client before the app starts. Requires the app to be stopped.
    App& useHttpClient(HttpClientConfig config);
    App& useHttpClient(std::string_view alias, HttpClientConfig config);
    // Register (or replace) / unregister an upstream HTTP client at RUNTIME, while the app is
    // running -- for a reverse proxy / CDN edge whose origins change dynamically. Thread-safe: the
    // change is posted to every worker's io_context. An existing alias is replaced; the old client
    // is closed and destroyed once its in-flight requests drain. Also updates the stored config so
    // a restart includes the change. Callable while stopped too (behaves like useHttpClient).
    App& addHttpClient(std::string_view alias, HttpClientConfig config);
    App& removeHttpClient(std::string_view alias);

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
