#pragma once

#ifdef RUVIA_ENABLE_DATABASE

#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/app/ConfigValidation.h"

namespace ruvia::detail {

template <typename Config>
inline void validateDbConfig(const Config& config) {
    ensureConfigHost(config.host, "database host must not be empty", "database host is invalid", kSeparatedPortHostRules);
    ensureNonZeroPort(config.port, "database port must not be zero");
    ensurePositiveOptionalDurations("configured database timeouts must be greater than zero", config.connectTimeout, config.readTimeout, config.writeTimeout, config.queryTimeout, config.acquireTimeout);

    switch (config.driver) {
        case DbDriver::kMariaDb:
#ifndef RUVIA_ENABLE_MARIADB
            throw std::invalid_argument("MariaDB support is not enabled");
#else
            break;
#endif
        case DbDriver::kPostgreSql:
#ifndef RUVIA_ENABLE_POSTGRESQL
            throw std::invalid_argument("PostgreSQL support is not enabled");
#else
            break;
#endif
        default:
            throw std::invalid_argument("database driver is invalid");
    }
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_DATABASE
