#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/core/ConfigValidation.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/web/detail/server/HttpServerListener.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/tls/TlsHost.h"

namespace ruvia::detail {

inline void validateHttpServerTlsIdentity(
    const HttpServerListenerDefinition::TlsIdentity& identity) {
    if (identity.certificateChainFile.empty() || identity.privateKeyFile.empty()) {
        throw std::invalid_argument(
            "TLS certificate chain and private key files must not be empty");
    }
}

inline void validateDocumentRootRuntimeConfig(const HttpServerOptions& options) {
    const auto* refresh = options.documentRoot.refreshOptions();
    if (refresh == nullptr) {
        return;
    }
    ruvia::ensurePositiveDuration(
        refresh->refreshInterval, "document root refresh interval must be greater than zero");
    if (options.blockingPool == nullptr) {
        throw std::invalid_argument(
            "document root refresh cannot run while the blocking pool is disabled");
    }
    const auto* precompression = options.documentRoot.precompressionOptions();
    if (precompression == nullptr) {
        return;
    }
    ruvia::ensurePositiveSize(precompression->minBytes,
        "document root precompression minimum size must be greater than zero");
    if (precompression->maxBytes < precompression->minBytes) {
        throw std::invalid_argument(
            "document root precompression maximum size must not be smaller than the minimum size");
    }
}

inline void validateHttpServerOptions(const HttpServerOptions& options) {
    ruvia::ensurePositiveOptionalDurations("configured server timeouts must be greater than zero",
        options.idleTimeout, options.requestHeaderTimeout, options.requestBodyTimeout,
        options.writeTimeout);
    ruvia::ensurePositiveDuration(options.scanInterval, "connection scan interval must be greater than 0");
    ruvia::ensurePositiveSize(
        options.workerMailboxCapacity, "worker mailbox capacity must be greater than 0");
    if (!std::has_single_bit(options.rateLimitCapacityPerWorker)) {
        throw std::invalid_argument("rate-limit capacity per worker must be a power of two");
    }
    ruvia::ensurePositiveSize(options.memoryConfig.requestInitialBufferBytes,
        "memory pool config values must be greater than 0");
    ruvia::ensurePositiveSize(options.maxBufferedBodyBytes, "buffered body limit must be greater than 0");
    ruvia::ensurePositiveOptionalSize(
        options.maxStreamBodyBytes, "configured stream body limit must be greater than zero");
    ruvia::ensurePositiveSize(
        options.maxWebSocketMessageBytes, "websocket message limit must be greater than 0");
    ruvia::ensurePositiveOptionalSize(
        options.maxConnections, "configured connection limit must be greater than zero");
    ruvia::ensurePositiveOptionalSize(options.maxRequestsPerConnection,
        "configured requests-per-connection limit must be greater than zero");
    if (options.compression.has_value()) {
        ruvia::ensurePositiveSize(
            options.compression->minBytes, "compression minimum size must be greater than zero");
        if (options.compression->syncBytes < options.compression->minBytes) {
            throw std::invalid_argument(
                "compression synchronous size must not be smaller than the minimum size");
        }
        if (options.compression->maxBytes < options.compression->syncBytes) {
            throw std::invalid_argument(
                "compression maximum size must not be smaller than the synchronous size");
        }
    }
    validateDocumentRootRuntimeConfig(options);
}

inline void validateHttpServerTlsOptions(const HttpServerListenerDefinition::Tls& tls) {
    validateHttpServerTlsIdentity(tls.identity);
    if (tls.clientCertificates.has_value()) {
        switch (tls.clientCertificates->requirement) {
            case TlsClientCertificateRequirement::kOptional:
            case TlsClientCertificateRequirement::kRequired:
                break;
            default:
                throw std::invalid_argument("TLS client certificate requirement is invalid");
        }
        if (tls.clientCertificates->verifyFile.empty()) {
            throw std::invalid_argument("TLS client certificate CA bundle must not be empty");
        }
    }
    for (std::size_t i = 0; i < tls.sniIdentities.size(); ++i) {
        const auto& sni = tls.sniIdentities[i];
        ensureSniHost(sni.host, "SNI host must not be empty", "SNI host is invalid");
        validateHttpServerTlsIdentity(sni.identity);
        for (std::size_t j = 0; j < i; ++j) {
            if (httpAsciiEqualsIgnoreCase(tls.sniIdentities[j].host, sni.host)) {
                throw std::invalid_argument("SNI hosts must be unique");
            }
        }
    }
}

inline constexpr std::size_t kHttp3PeerUnidirectionalStreamAllowance = 64;
inline constexpr std::size_t kHttp3PostGoawayRequestAllowance = 64;

[[nodiscard]] inline std::size_t http3WorkerTrackedStreamCapacity(
    std::size_t maxRequestsPerConnection) {
    constexpr auto allowance = kHttp3PeerUnidirectionalStreamAllowance;
    if (maxRequestsPerConnection > std::numeric_limits<std::size_t>::max() - allowance) {
        throw std::invalid_argument("HTTP/3 worker stream capacity is not representable");
    }
    return maxRequestsPerConnection + allowance;
}

[[nodiscard]] inline std::size_t http3TransportLifetimeStreamCapacity(
    std::size_t maxRequestsPerConnection) {
    constexpr auto allowance = kHttp3PeerUnidirectionalStreamAllowance +
                               kHttp3PostGoawayRequestAllowance;
    if (maxRequestsPerConnection > std::numeric_limits<std::size_t>::max() - allowance) {
        throw std::invalid_argument("HTTP/3 transport stream capacity is not representable");
    }
    return maxRequestsPerConnection + allowance;
}

inline void validateHttp3ServerLimits(
    std::optional<std::size_t> maxConnections, std::size_t workerMailboxCapacity,
    std::optional<std::size_t> maxRequestsPerConnection, std::size_t workerCount) {
    if (workerCount == 0) {
        throw std::invalid_argument("HTTP/3 worker count must be greater than zero");
    }
    if (!maxConnections.has_value()) {
        throw std::invalid_argument("HTTP/3 requires a finite per-worker connection limit");
    }
    ruvia::ensurePositiveSize(*maxConnections,
        "HTTP/3 per-worker connection limit must be greater than zero");

    // Keep the aggregate slot count representable as a container difference and
    // as the network transport's total active-connection capacity.
    const auto maxConnectionCapacity =
        static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    if (*maxConnections > maxConnectionCapacity / workerCount) {
        throw std::invalid_argument(
            "HTTP/3 aggregate connection capacity is not representable");
    }

    // The stream output uses UINT32_MAX as its free-list sentinel, so a mailbox
    // capacity equal to that value is not a valid preallocated node count.
    if (workerMailboxCapacity >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(
            "HTTP/3 worker mailbox capacity must be below the 32-bit node limit");
    }

    if (!maxRequestsPerConnection.has_value()) {
        throw std::invalid_argument(
            "HTTP/3 requires a finite per-connection request limit");
    }
    ruvia::ensurePositiveSize(*maxRequestsPerConnection,
        "HTTP/3 per-connection request limit must be greater than zero");

    // The request-stream boundary is a client-bidi QUIC stream ID: 4 * N.
    constexpr std::uint64_t maxGoawayId = (std::uint64_t{1} << 62) - 4;
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (*maxRequestsPerConnection > std::numeric_limits<std::uint64_t>::max()) {
            throw std::invalid_argument("HTTP/3 request limit does not fit a GOAWAY varint");
        }
    }
    const auto requestLimit = static_cast<std::uint64_t>(*maxRequestsPerConnection);
    if (requestLimit > maxGoawayId / 4) {
        throw std::invalid_argument("HTTP/3 GOAWAY request boundary is not representable");
    }

    const auto trackedStreams =
        http3WorkerTrackedStreamCapacity(*maxRequestsPerConnection);
    const auto lifetimeStreams =
        http3TransportLifetimeStreamCapacity(*maxRequestsPerConnection);
    constexpr auto maxPowerOfTwo =
        std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (trackedStreams > maxPowerOfTwo / 2 || lifetimeStreams > maxPowerOfTwo) {
        throw std::invalid_argument("HTTP/3 stream tracking capacity is not representable");
    }
    const std::vector<std::size_t> slots;
    if (trackedStreams > slots.max_size() / 2 || lifetimeStreams > slots.max_size()) {
        throw std::invalid_argument("HTTP/3 stream capacity exceeds container limits");
    }
}

inline void validateHttpServerListener(const HttpServerListenerDefinition& listener) {
    if (const auto* tls = std::get_if<HttpServerListenerDefinition::Tls>(&listener.transport)) {
        validateHttpServerTlsOptions(*tls);
    } else if (listener.http3.has_value()) {
        throw std::invalid_argument("HTTP/3 listener requires TLS");
    }
    if (listener.http3.has_value()) {
        ruvia::ensurePositiveDuration(listener.http3->handshakeTimeout,
            "HTTP/3 handshake timeout must be greater than zero");
        ruvia::ensurePositiveDuration(listener.http3->drainTimeout,
            "HTTP/3 drain timeout must be greater than zero");
        if (std::chrono::duration<long double>(listener.http3->drainTimeout) >
            std::chrono::duration<long double>(std::chrono::steady_clock::duration::max())) {
            throw std::invalid_argument("HTTP/3 drain timeout is not representable");
        }
    }
    if (const auto* redirect =
            std::get_if<HttpServerListenerDefinition::RedirectHttpToHttps>(&listener.transport)) {
        ruvia::ensureNonZeroPort(
            redirect->httpsPort, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
    }
}

class ValidatedHttpServerConfiguration final {
public:
    [[nodiscard]] std::span<const HttpServerListenerDefinition> listeners() const noexcept {
        return listeners_;
    }

    [[nodiscard]] const HttpServerOptions& options() const noexcept {
        return options_;
    }

private:
    ValidatedHttpServerConfiguration(
        std::span<const HttpServerListenerDefinition> listeners, HttpServerOptions&& options)
        : listeners_(listeners),
          options_(std::move(options)) {}

    friend ValidatedHttpServerConfiguration validateHttpServerConfiguration(
        std::span<const HttpServerListenerDefinition> listeners, HttpServerOptions&& options);

    std::span<const HttpServerListenerDefinition> listeners_;
    HttpServerOptions options_;
};

[[nodiscard]] inline ValidatedHttpServerConfiguration validateHttpServerConfiguration(
    std::span<const HttpServerListenerDefinition> listeners, HttpServerOptions&& options) {
    if (listeners.empty()) {
        throw std::invalid_argument("HTTP server worker requires at least one listener");
    }
    bool hasHttp3 = false;
    for (const auto& listener : listeners) {
        validateHttpServerListener(listener);
        if (listener.http3.has_value()) {
            if (hasHttp3) {
                throw std::invalid_argument(
                    "only one HTTP/3 listener is supported by the App runtime");
            }
            hasHttp3 = true;
        }
    }
    validateHttpServerOptions(options);
    if (hasHttp3) {
        validateHttp3ServerLimits(
            options.maxConnections, options.workerMailboxCapacity,
            options.maxRequestsPerConnection, 1);
    }
    return ValidatedHttpServerConfiguration(listeners, std::move(options));
}

}  // namespace ruvia::detail
