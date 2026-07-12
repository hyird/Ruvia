#pragma once

#include <chrono>
#include <stdexcept>
#include <string_view>

#include "ruvia/web/ServerConfig.h"
#include "ruvia/http/HttpHeader.h"

namespace ruvia::detail {

inline void validateCorsHeaderValue(std::string_view value, const char* message) {
    if (!isValidHttpHeaderValue(value)) {
        throw std::invalid_argument(message);
    }
}

inline void validateCorsFields(
    std::string_view allowOrigin,
    std::string_view allowHeaders,
    std::string_view exposeHeaders,
    std::chrono::seconds maxAge,
    bool allowCredentials) {
    if (allowOrigin.empty()) {
        throw std::invalid_argument("CORS allowOrigin must not be empty");
    }
    // A wildcard origin combined with credentials would force reflecting the
    // request's Origin back with Access-Control-Allow-Credentials: true, letting
    // any site read credentialed responses. Require an explicit single origin for
    // credentialed CORS instead of silently reflecting every origin.
    if (allowCredentials && allowOrigin == "*") {
        throw std::invalid_argument(
            "CORS allowCredentials requires an explicit allowOrigin, not \"*\"");
    }
    validateCorsHeaderValue(allowOrigin, "CORS allowOrigin must be a valid header value");
    validateCorsHeaderValue(allowHeaders, "CORS allowHeaders must be a valid header value");
    validateCorsHeaderValue(exposeHeaders, "CORS exposeHeaders must be a valid header value");
    if (maxAge.count() < 0) {
        throw std::invalid_argument("CORS maxAge must not be negative");
    }
}

inline void validateCorsConfig(const CorsConfig& config) {
    validateCorsFields(
        config.allowOrigin,
        config.allowHeaders,
        config.exposeHeaders,
        config.maxAge,
        config.allowCredentials);
}

}  // namespace ruvia::detail
