#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/RateLimitRule.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/web/StaticFiles.h"

// Web server runtime configuration and its per-request access-log record. These
// types configure the ruvia-web server driver and intentionally live under the Web
// public header root; App re-exports them by including this header.

namespace ruvia {

namespace detail {
struct AccessLogRecordAccess;
}  // namespace detail

// One terminal response outcome with a committed final status, passed to the
// access-log callback after a complete buffered response head has reached the
// transport or a stream head is committed. The record borrows the immutable
// request and connection-owned remote address; the record and all returned views
// are valid only for the callback.
class AccessLogRecord final {
public:
    [[nodiscard]] std::string_view method() const noexcept {
        return request_.method();
    }

    [[nodiscard]] HttpKnownMethod knownMethod() const noexcept {
        return request_.knownMethod();
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return request_.path();
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

    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept {
        return request_.protocolVersion();
    }

private:
    friend struct detail::AccessLogRecordAccess;

    constexpr AccessLogRecord(
        const HttpRequest& request,
        std::string_view remoteAddress,
        std::uint16_t status,
        std::uint64_t durationMicros) noexcept
        : request_(request),
          remoteAddress_(remoteAddress),
          status_(status),
          durationMicros_(durationMicros) {}

    const HttpRequest& request_;
    std::string_view remoteAddress_;
    std::uint16_t status_;
    std::uint64_t durationMicros_;
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
    // unset and never allocates. Invoked once after a final response head is
    // committed. A peer/transport failure before the complete final head has
    // reached the wire has no HTTP status and does not invoke this hook.
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

}  // namespace ruvia
