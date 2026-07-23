#pragma once

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

#include "ruvia/core/detail/util/FailureReport.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/RateLimitRule.h"
#include "ruvia/web/ServerConfig.h"

namespace ruvia {
class Env;
}

namespace ruvia::detail {

struct AccessLogSink final {
    AccessLogCallback callback;

    void invoke(const AccessLogRecord& record) const noexcept {
        callback.invoke(record);
    }
};

struct ConnectionFailureRecordAccess final {
    [[nodiscard]] static ConnectionFailureRecord make(
        std::string_view remoteAddress,
        std::exception_ptr exception) noexcept {
        return ConnectionFailureRecord(remoteAddress, std::move(exception));
    }
};

// The connection level's failure outlet. An unset callback still reports: the
// failure goes to the shared last-resort reporter rather than being dropped
// with the connection that produced it.
struct ConnectionFailureSink final {
    ConnectionFailureCallback callback;

    void invoke(
        std::string_view remoteAddress,
        std::exception_ptr exception) const noexcept {
        if (exception == nullptr) {
            return;
        }
        if (callback) {
            callback.invoke(
                ConnectionFailureRecordAccess::make(
                    remoteAddress, std::move(exception)));
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
        std::pmr::string certificateChainFile;
        std::pmr::string privateKeyFile;
        std::pmr::string privateKeyPassword;
    };

    struct TlsClientCertificatePolicy final {
        std::pmr::string verifyFile;
        ruvia::TlsClientCertificateRequirement requirement{
            ruvia::TlsClientCertificateRequirement::kOptional};
    };

    struct Tls final {
        // An additional certificate selected by SNI server name (RFC 6066).
        struct SniIdentity final {
            std::pmr::string host;
            TlsIdentity identity;
        };

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
    std::optional<CorsConfig> cors;
    DocumentRoot documentRoot;
    AccessLogSink accessLog;
    const Env* env{nullptr};
    WorkerFailureSink workerFailure;
    ConnectionFailureSink connectionFailure;
    std::optional<RateLimitRule> defaultRateLimitPerWorker;
    std::size_t rateLimitSlotsPerWorker{kDefaultRateLimitSlotsPerWorker};

    [[nodiscard]] const Tls* tls() const & noexcept {
        return std::get_if<Tls>(&transport);
    }
    const Tls* tls() const && = delete;

    [[nodiscard]] const RedirectHttpToHttps* redirect() const & noexcept {
        return std::get_if<RedirectHttpToHttps>(&transport);
    }
    const RedirectHttpToHttps* redirect() const && = delete;
};

}  // namespace ruvia::detail
