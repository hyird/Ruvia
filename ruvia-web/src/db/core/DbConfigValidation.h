#pragma once

#ifdef RUVIA_ENABLE_MARIADB

#include "ruvia/db/DbTypes.h"
#include "ConfigValidation.h"

namespace ruvia::detail {

inline void validateDbConfig(const DbConfig& config) {
    ensureConfigHost(
        config.host,
        "database host must not be empty",
        "database host is invalid",
        kSeparatedPortHostRules);
    ensureNonZeroPort(config.port, "database port must not be zero");
    ensurePositiveSize(config.poolSize, "database pool size must be greater than zero");
    ensureNonNegativeDurations(
        "database timeouts must not be negative",
        config.connectTimeout,
        config.readTimeout,
        config.writeTimeout,
        config.queryTimeout,
        config.acquireTimeout);
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_MARIADB
