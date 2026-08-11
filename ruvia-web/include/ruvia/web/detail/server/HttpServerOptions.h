#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/core/detail/util/FailureReport.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/RateLimitRule.h"
#include "ruvia/web/ServerConfig.h"
#include "ruvia/web/detail/server/TrustedProxies.h"
#include "ruvia/web/detail/http/CorsOptions.h"
#include "ruvia/web/detail/server/DocumentRootBinding.h"

namespace ruvia {
class Env;
}

namespace ruvia::detail {

struct AccessLogSink final {
    AccessLogCallbackRef callback;

    void invoke(const AccessLogRecord& record) const noexcept {
        callback(record);
    }
};

struct ConnectionFailureRecordAccess final {
    [[nodiscard]] static ConnectionFailureRecord make(std::string_view remoteAddress, std::exception_ptr exception) noexcept {
        return ConnectionFailureRecord(remoteAddress, std::move(exception));
    }
};

// The connection level's failure outlet. An unset callback still reports: the
// failure goes to the shared last-resort reporter rather than being dropped
// with the connection that produced it.
struct ConnectionFailureSink final {
    ConnectionFailureCallbackRef callback;
    // Owned by the HttpServer this sink was configured for; null before one
    // claims it. Counting here rather than at each reporting site keeps the
    // count and the callback from drifting apart as new sites are added.
    std::atomic<std::size_t>* counter{nullptr};

    void invoke(std::string_view remoteAddress, std::exception_ptr exception) const noexcept {
        if (exception == nullptr) {
            return;
        }
        if (counter != nullptr) {
            counter->fetch_add(1, std::memory_order_relaxed);
        }
        if (callback) {
            callback(ConnectionFailureRecordAccess::make(remoteAddress, std::move(exception)));
            return;
        }
        reportUnhandledFailure("web connection", std::move(exception));
    }
};

struct WorkerFailureSink final {
    using Invoke = void (*)(void*, std::exception_ptr) noexcept;

    void* target{nullptr};
    Invoke invoke{nullptr};

    void notify(std::exception_ptr failure) const noexcept {
        if (invoke != nullptr) {
            invoke(target, std::move(failure));
        }
    }
};

// Fully normalized, worker-owned runtime options. Public callers configure App
// with ServerConfig.h values; only the Web runtime constructs this state.
struct HttpServerOptions final {
    struct TlsIdentity final {
        explicit TlsIdentity(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
            : certificateChainFile(resource), privateKeyFile(resource), privateKeyPassword(resource) {}

        std::pmr::string certificateChainFile;
        std::pmr::string privateKeyFile;
        std::pmr::string privateKeyPassword;
    };

    struct TlsClientCertificatePolicy final {
        explicit TlsClientCertificatePolicy(std::pmr::memory_resource* resource = std::pmr::get_default_resource(), ruvia::TlsClientCertificateRequirement configuredRequirement = ruvia::TlsClientCertificateRequirement::kOptional)
            : verifyFile(resource), requirement(configuredRequirement) {}

        std::pmr::string verifyFile;
        ruvia::TlsClientCertificateRequirement requirement{ruvia::TlsClientCertificateRequirement::kOptional};
    };

    struct Tls final {
        // An additional certificate selected by SNI server name (RFC 6066).
        struct SniIdentity final {
            explicit SniIdentity(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
                : host(resource), identity(resource) {}

            std::pmr::string host;
            TlsIdentity identity;
        };

        explicit Tls(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
            : identity(resource), sniIdentities(resource) {}

        TlsIdentity identity;
        std::optional<TlsClientCertificatePolicy> clientCertificates;
        std::pmr::vector<SniIdentity> sniIdentities;
    };

    struct PlainHttp final {};

    struct RedirectHttpToHttps final {
        std::uint16_t httpsPort;
    };

    using ListenerTransport = std::variant<PlainHttp, Tls, RedirectHttpToHttps>;

    struct DocumentRoot final {
        const StaticRoot* root{nullptr};
        DocumentRootRuntimeOptions runtimeOptions;

        // Keep the root and its runtime policy coupled at the dispatch
        // boundary; a null root produces the explicit no-root state.
        [[nodiscard]] DocumentRootBinding binding() const noexcept {
            if (root == nullptr) {
                return DocumentRootBinding::none();
            }
            return DocumentRootBinding::configured(*root, runtimeOptions);
        }
    };

    // nginx-aligned inactivity timeouts. Absence disables a phase timeout;
    // keepaliveRequests caps requests per reused connection.
    std::optional<std::chrono::milliseconds> keepaliveTimeout{std::chrono::seconds(75)};
    std::chrono::milliseconds scanInterval{std::chrono::seconds(1)};
    // Capacity of the explicit cross-thread queue for this Web worker.
    std::size_t workerMailboxCapacity{1024};
    MemoryPoolConfig memoryConfig{};
    std::optional<std::chrono::milliseconds> clientHeaderTimeout{std::chrono::seconds(60)};
    std::optional<std::chrono::milliseconds> clientBodyTimeout{std::chrono::seconds(60)};
    std::optional<std::chrono::milliseconds> sendTimeout{std::chrono::seconds(60)};
    // Per worker. Defaults to a bounded cap so an unconfigured server cannot be
    // driven to FD/memory exhaustion by a connection flood; set std::nullopt to
    // opt back into unlimited. Excess accepted sockets are closed before TLS or
    // HTTP protocol detection, so admission never fabricates a response.
    std::optional<std::size_t> maxConnections{1024};
    std::optional<std::size_t> keepaliveRequests{1000};
    // Buffered routes materialize body data; the same cap applies again after
    // Content-Encoding is decoded. This limit must be greater than 0.
    std::size_t maxBufferedBodyBytes{kDefaultMaxBufferedBodyBytes};
    // Stream routes are explicit; absence disables the stream body limit.
    std::optional<std::size_t> maxStreamBodyBytes;
    // WebSocket messages are assembled before delivery; this must be greater than 0.
    std::size_t maxWebSocketMessageBytes{kDefaultMaxWebSocketMessageBytes};
    ListenerTransport transport;
    // Presence enables the policy; absence bypasses it without retaining an
    // inactive configuration state.
    std::optional<CompressionConfig> compression{std::in_place};
    std::optional<CorsOptions> cors;
    DocumentRoot documentRoot;
    // Peers whose forwarding headers may be believed. Empty by default, so an
    // unconfigured server treats every direct peer as the client.
    TrustedProxySet trustedProxies;
    AccessLogSink accessLog;
    const Env* env{nullptr};
    // Process-wide, owned by App::run() and shared by every worker. Null when
    // the app configured no pool; Context::runBlocking() then reports the
    // missing configuration instead of blocking the worker.
    BlockingPool* blockingPool{nullptr};
    WorkerFailureSink workerFailure;
    ConnectionFailureSink connectionFailure;
    std::optional<RateLimitRule> defaultRateLimitPerWorker;
    std::size_t rateLimitSlotsPerWorker{kDefaultRateLimitSlotsPerWorker};

    [[nodiscard]] const Tls* tls() const& noexcept {
        return std::get_if<Tls>(&transport);
    }
    const Tls* tls() const&& = delete;

    [[nodiscard]] const RedirectHttpToHttps* redirect() const& noexcept {
        return std::get_if<RedirectHttpToHttps>(&transport);
    }
    const RedirectHttpToHttps* redirect() const&& = delete;
};

}  // namespace ruvia::detail
