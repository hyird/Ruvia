#pragma once

#include <stdexcept>

#include "ruvia/web/detail/app/ConfigValidation.h"
#include "ruvia/web/detail/http/HttpCorsConfigValidation.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"

namespace ruvia::detail {

inline void validateHttpServerOptions(const HttpServerOptions& options) {
    ensureNonNegativeDurations(
        "server timeouts must not be negative",
        options.keepaliveTimeout,
        options.clientHeaderTimeout,
        options.clientBodyTimeout,
        options.sendTimeout);
    ensurePositiveDuration(options.scanInterval, "connection scan interval must be greater than 0");
    ensurePositiveSize(options.maxBufferedBodyBytes, "buffered body limit must be greater than 0");
    ensurePositiveSize(options.maxWebSocketMessageBytes, "websocket message limit must be greater than 0");
    if (options.tls.enabled &&
        (options.tls.certificateChainFile.empty() || options.tls.privateKeyFile.empty())) {
        throw std::invalid_argument("TLS certificate chain and private key files must not be empty");
    }
    if (options.autoHttps.enabled) {
        ensureNonZeroPort(options.autoHttps.httpsPort, "auto HTTPS requires a fixed HTTPS listen port");
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
