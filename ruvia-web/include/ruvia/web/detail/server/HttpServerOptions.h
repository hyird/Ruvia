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

    class DocumentRoot final {
        struct Standalone final {
            const StaticRoot* root;
        };
        struct Refreshing final {
            const StaticRoot* root;
            DocumentRootRuntimeOptions options;
        };

    public:
        DocumentRoot() noexcept = default;

        [[nodiscard]] static DocumentRoot standalone(const StaticRoot& root) noexcept {
            return DocumentRoot(Standalone{&root});
        }

        [[nodiscard]] static DocumentRoot refreshing(
            const StaticRoot& root,
            DocumentRootRuntimeOptions options = {}) noexcept {
            return DocumentRoot(Refreshing{&root, options});
        }

        [[nodiscard]] const StaticRoot* root() const noexcept {
            if (const auto* standalone = std::get_if<Standalone>(&state_)) return standalone->root;
            if (const auto* refreshing = std::get_if<Refreshing>(&state_)) return refreshing->root;
            return nullptr;
        }

        [[nodiscard]] const DocumentRootRuntimeOptions* refreshOptions() const noexcept {
            const auto* refreshing = std::get_if<Refreshing>(&state_);
            return refreshing == nullptr ? nullptr : &refreshing->options;
        }

        // Publishes the next immutable snapshot without changing the ownership
        // state. Calling this on a non-refreshing root is an invariant breach.
        void publish(const StaticRoot& root) noexcept {
            auto* refreshing = std::get_if<Refreshing>(&state_);
            if (refreshing == nullptr) std::terminate();
            refreshing->root = &root;
        }

        [[nodiscard]] DocumentRootBinding binding() const noexcept {
            if (const auto* standalone = std::get_if<Standalone>(&state_)) {
                return DocumentRootBinding::standalone(*standalone->root);
            }
            if (const auto* refreshing = std::get_if<Refreshing>(&state_)) {
                return DocumentRootBinding::configured(*refreshing->root);
            }
            return DocumentRootBinding::none();
        }

    private:
        explicit DocumentRoot(Standalone state) noexcept : state_(state) {}
        explicit DocumentRoot(Refreshing state) noexcept : state_(state) {}

        std::variant<std::monostate, Standalone, Refreshing> state_;
    };

    // nginx-aligned inactivity timeouts. Absence disables a phase timeout;
    // maxRequestsPerConnection caps requests per reused connection.
    std::optional<std::chrono::milliseconds> idleTimeout{std::chrono::seconds(75)};
    std::chrono::milliseconds scanInterval{std::chrono::seconds(1)};
    // Capacity of the explicit cross-thread queue for this Web worker.
    std::size_t workerMailboxCapacity{1024};
    std::size_t httpClientOriginCacheCapacityPerWorker{64};
    MemoryPoolConfig memoryConfig{};
    std::optional<std::chrono::milliseconds> requestHeaderTimeout{std::chrono::seconds(60)};
    std::optional<std::chrono::milliseconds> requestBodyTimeout{std::chrono::seconds(60)};
    std::optional<std::chrono::milliseconds> writeTimeout{std::chrono::seconds(60)};
    // Per worker. Defaults to a bounded cap so an unconfigured server cannot be
    // driven to FD/memory exhaustion by a connection flood; set std::nullopt to
    // opt back into unlimited. Excess accepted sockets are closed before TLS or
    // HTTP protocol detection, so admission never fabricates a response.
    std::optional<std::size_t> maxConnections{1024};
    std::optional<std::size_t> maxRequestsPerConnection{1000};
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
    std::optional<CompressionConfig> compression;
    std::optional<CorsOptions> cors;
    DocumentRoot documentRoot;
    // Peers whose forwarding headers may be believed. Empty by default, so an
    // unconfigured server treats every direct peer as the client.
    // Absent means no handler deadline anywhere, and nothing is armed.
    std::optional<DeadlineConfig> deadline;
    TrustedProxySet trustedProxies;
    AccessLogSink accessLog;
    const Env* env{nullptr};
    // Process-wide, owned by App::run() and shared by every worker. Null only
    // when the app explicitly disabled the default pool; runBlocking() then
    // reports that state instead of blocking the worker.
    BlockingPool* blockingPool{nullptr};
    WorkerFailureSink workerFailure;
    ConnectionFailureSink connectionFailure;
    std::optional<RateLimitRule> defaultRateLimitPerWorker;
    std::size_t rateLimitCapacityPerWorker{kDefaultRateLimitCapacityPerWorker};

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
