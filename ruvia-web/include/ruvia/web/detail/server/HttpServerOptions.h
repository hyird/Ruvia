#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/RateLimitRule.h"
#include "ruvia/web/ServerConfig.h"

namespace ruvia::detail {

struct AccessLogSink final {
    AccessLogCallback callback{nullptr};
    void* user{nullptr};
};

// Fully normalized, worker-owned runtime options. Public callers configure App
// with ServerConfig.h values; only the Web runtime constructs this state.
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

    struct DocumentRoot final {
        const StaticRoot* root{nullptr};
    };

    struct AutoHttps final {
        bool enabled{false};
        std::uint16_t httpsPort{443};
    };

    // nginx-aligned inactivity timeouts. Set any timeout except scanInterval
    // to 0 to disable it; keepaliveRequests caps requests per reused connection.
    std::chrono::milliseconds keepaliveTimeout{std::chrono::seconds(75)};
    std::chrono::milliseconds shutdownGracePeriod{0};
    std::chrono::milliseconds scanInterval{std::chrono::seconds(1)};
    std::chrono::milliseconds clientHeaderTimeout{std::chrono::seconds(60)};
    std::chrono::milliseconds clientBodyTimeout{std::chrono::seconds(60)};
    std::chrono::milliseconds sendTimeout{std::chrono::seconds(60)};
    // Per worker. 0 is unlimited; excess accepted sockets are closed before
    // TLS or HTTP protocol detection, so admission never fabricates a response.
    std::size_t maxConnections{0};
    std::size_t keepaliveRequests{1000};
    // Buffered routes materialize body data; the same cap applies again after
    // Content-Encoding is decoded. This limit must be greater than 0.
    std::size_t maxBufferedBodyBytes{kDefaultMaxBufferedBodyBytes};
    // Stream routes are explicit; 0 disables the stream body limit.
    std::size_t maxStreamBodyBytes{kDefaultMaxStreamBodyBytes};
    // WebSocket messages are assembled before delivery; this must be greater than 0.
    std::size_t maxWebSocketMessageBytes{kDefaultMaxWebSocketMessageBytes};
    Tls tls;
    // Presence enables the policy; absence bypasses it without retaining an
    // inactive configuration state.
    std::optional<CompressionConfig> compression{std::in_place};
    std::optional<CorsConfig> cors;
    DocumentRoot documentRoot;
    AutoHttps autoHttps;
    AccessLogSink accessLog;
    std::optional<RateLimitRule> rateLimit;
};

}  // namespace ruvia::detail
