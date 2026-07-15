#pragma once

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

inline void validateCorsConfig(const CorsConfig& config) {
    validateCorsHeaderValue(
        config.exposeHeaders,
        "CORS exposeHeaders must be a valid header value");
}

}  // namespace ruvia::detail
