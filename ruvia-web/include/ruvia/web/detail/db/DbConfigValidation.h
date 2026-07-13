#pragma once

#ifdef RUVIA_ENABLE_MARIADB

#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/app/ConfigValidation.h"

namespace ruvia::detail {

inline void validateDbConfig(const DbConfig& config) {
    ensureConfigHost(
        config.host,
        "database host must not be empty",
        "database host is invalid",
        kSeparatedPortHostRules);
    ensureNonZeroPort(config.port, "database port must not be zero");
    ensurePositiveSize(config.poolSize, "database pool size must be greater than zero");
    ensurePositiveOptionalDurations(
        "configured database timeouts must be greater than zero",
        config.connectTimeout,
        config.readTimeout,
        config.writeTimeout,
        config.queryTimeout,
        config.acquireTimeout);
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_MARIADB
