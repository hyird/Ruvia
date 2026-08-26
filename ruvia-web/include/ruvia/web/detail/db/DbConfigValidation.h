#pragma once

#include "ruvia/web/db/DbTypes.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/core/detail/config/ConfigValidation.h"
#endif

namespace ruvia::detail {

[[nodiscard]] inline constexpr std::uint16_t defaultDbPort(DbDriver driver) noexcept {
    switch (driver) {
        case DbDriver::kMariaDb:
            return 3306;
        case DbDriver::kPostgreSql:
            return 5432;
        default:
            return 0;
    }
}

#ifdef RUVIA_ENABLE_DATABASE

template <typename Config>
[[nodiscard]] inline DbDriver configuredDbDriver(const Config& config) noexcept {
    return config.driver;
}

template <typename Config>
[[nodiscard]] inline std::uint16_t configuredDbPort(const Config& config) noexcept {
    if constexpr (requires { config.port.has_value(); }) {
        return config.port.value_or(defaultDbPort(configuredDbDriver(config)));
    } else {
        return config.port;
    }
}

template <typename Config>
inline void validateDbConfig(const Config& config) {
    switch (configuredDbDriver(config)) {
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
            throw std::invalid_argument("database driver must be selected");
    }

    ensureConfigHost(config.host, "database host must not be empty", "database host is invalid",
        kSeparatedPortHostRules);
    ensureNonZeroPort(configuredDbPort(config), "database port must not be zero");
    ensurePositiveOptionalDurations("configured database timeouts must be greater than zero",
        config.connectTimeout, config.readTimeout, config.writeTimeout, config.queryTimeout,
        config.acquireTimeout);
}

#endif  // RUVIA_ENABLE_DATABASE

}  // namespace ruvia::detail
