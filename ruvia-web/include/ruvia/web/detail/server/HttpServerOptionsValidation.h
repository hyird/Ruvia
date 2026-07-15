#pragma once

#include <stdexcept>

#include "ruvia/web/detail/app/ConfigValidation.h"
#include "ruvia/web/detail/http/HttpCorsConfigValidation.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"

namespace ruvia::detail {

inline void validateHttpServerOptions(const HttpServerOptions& options) {
    ensurePositiveOptionalDurations(
        "configured server timeouts must be greater than zero",
        options.keepaliveTimeout,
        options.clientHeaderTimeout,
        options.clientBodyTimeout,
        options.sendTimeout);
    ensurePositiveDuration(options.scanInterval, "connection scan interval must be greater than 0");
    ensurePositiveSize(options.maxBufferedBodyBytes, "buffered body limit must be greater than 0");
    ensurePositiveOptionalSize(
        options.maxStreamBodyBytes,
        "configured stream body limit must be greater than zero");
    ensurePositiveSize(options.maxWebSocketMessageBytes, "websocket message limit must be greater than 0");
    ensurePositiveOptionalSize(
        options.maxConnections,
        "configured connection limit must be greater than zero");
    ensurePositiveOptionalSize(
        options.keepaliveRequests,
        "configured keepalive request limit must be greater than zero");
    if (const auto* tls = options.tls();
        tls != nullptr &&
        (tls->identity.certificateChainFile.empty() ||
         tls->identity.privateKeyFile.empty())) {
        throw std::invalid_argument("TLS certificate chain and private key files must not be empty");
    }
    if (const auto* redirect = options.redirect()) {
        ensureNonZeroPort(
            redirect->httpsPort,
            "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
    }
    if (options.cors.has_value()) {
        validateCorsConfig(*options.cors);
    }
}

[[nodiscard]] inline HttpServerOptions validatedHttpServerOptions(HttpServerOptions options) {
    validateHttpServerOptions(options);
    return options;
}

}  // namespace ruvia::detail
