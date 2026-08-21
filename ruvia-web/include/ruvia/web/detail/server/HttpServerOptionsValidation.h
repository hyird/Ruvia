#pragma once

#include <bit>
#include <stdexcept>

#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/web/detail/app/ConfigValidation.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"

namespace ruvia::detail {

inline void validateHttpServerTlsIdentity(const HttpServerOptions::TlsIdentity& identity) {
    if (identity.certificateChainFile.empty() || identity.privateKeyFile.empty()) {
        throw std::invalid_argument("TLS certificate chain and private key files must not be empty");
    }
}

inline void validateDocumentRootRuntimeOptions(const HttpServerOptions& options) {
    const auto* refresh = options.documentRoot.refreshOptions();
    if (refresh == nullptr) return;
    ensurePositiveDuration(refresh->refreshInterval, "document root refresh interval must be greater than zero");
    if (options.blockingPool == nullptr) {
        throw std::invalid_argument("document root refresh cannot run while the blocking pool is disabled");
    }
}

inline void validateHttpServerOptions(const HttpServerOptions& options) {
    ensurePositiveOptionalDurations("configured server timeouts must be greater than zero", options.idleTimeout, options.requestHeaderTimeout, options.requestBodyTimeout, options.writeTimeout);
    ensurePositiveDuration(options.scanInterval, "connection scan interval must be greater than 0");
    ensurePositiveSize(options.workerMailboxCapacity, "worker mailbox capacity must be greater than 0");
    if (!std::has_single_bit(options.rateLimitCapacityPerWorker)) {
        throw std::invalid_argument("rate-limit capacity per worker must be a power of two");
    }
    ensurePositiveSize(options.memoryConfig.requestInitialBufferBytes, "memory pool config values must be greater than 0");
    ensurePositiveSize(options.maxBufferedBodyBytes, "buffered body limit must be greater than 0");
    ensurePositiveSize(options.httpClientOriginCacheCapacityPerWorker, "HTTP client origin cache capacity must be greater than 0");
    ensurePositiveOptionalSize(options.maxStreamBodyBytes, "configured stream body limit must be greater than zero");
    ensurePositiveSize(options.maxWebSocketMessageBytes, "websocket message limit must be greater than 0");
    ensurePositiveOptionalSize(options.maxConnections, "configured connection limit must be greater than zero");
    ensurePositiveOptionalSize(options.maxRequestsPerConnection, "configured requests-per-connection limit must be greater than zero");
    if (options.compression.has_value()) {
        ensurePositiveSize(options.compression->minBytes, "compression minimum size must be greater than zero");
        if (options.compression->syncBytes < options.compression->minBytes) {
            throw std::invalid_argument("compression synchronous size must not be smaller than the minimum size");
        }
        if (options.compression->maxBytes < options.compression->syncBytes) {
            throw std::invalid_argument("compression maximum size must not be smaller than the synchronous size");
        }
    }
    validateDocumentRootRuntimeOptions(options);
    if (const auto* tls = options.tls()) {
        validateHttpServerTlsIdentity(tls->identity);
        if (tls->clientCertificates.has_value()) {
            if (tls->clientCertificates->verifyFile.empty()) {
                throw std::invalid_argument("TLS client certificate CA bundle must not be empty");
            }
            switch (tls->clientCertificates->requirement) {
                case TlsClientCertificateRequirement::kOptional:
                case TlsClientCertificateRequirement::kRequired:
                    break;
                default:
                    throw std::invalid_argument("TLS client certificate requirement is invalid");
            }
        }
        for (std::size_t i = 0; i < tls->sniIdentities.size(); ++i) {
            const auto& sni = tls->sniIdentities[i];
            ensureSniHost(sni.host, "SNI host must not be empty", "SNI host is invalid");
            validateHttpServerTlsIdentity(sni.identity);
            for (std::size_t j = 0; j < i; ++j) {
                if (httpAsciiEqualsIgnoreCase(tls->sniIdentities[j].host, sni.host)) {
                    throw std::invalid_argument("SNI hosts must be unique");
                }
            }
        }
    }
    if (const auto* redirect = options.redirect()) {
        ensureNonZeroPort(redirect->httpsPort, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
    }
}

[[nodiscard]] inline HttpServerOptions validatedHttpServerOptions(HttpServerOptions options) {
    validateHttpServerOptions(options);
    return options;
}

}  // namespace ruvia::detail
