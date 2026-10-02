#pragma once

#include <system_error>

#include <asio/ip/address.hpp>

#include "ruvia/core/ConfigValidation.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/client/ClientTlsConfigStorage.h"

namespace ruvia::detail {

class ValidatedDbConfigView final {
public:
    [[nodiscard]] const DbConfig& get() const noexcept {
        return *config_;
    }

private:
    friend ValidatedDbConfigView validatedDbConfig(const DbConfig& config);

    explicit ValidatedDbConfigView(const DbConfig& config) noexcept
        : config_(&config) {}

    const DbConfig* config_;
};

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

[[nodiscard]] inline std::uint16_t configuredDbPort(const DbConfig& config) noexcept {
    return config.port.value_or(defaultDbPort(config.driver));
}

inline void validateDbConfig(const DbConfig& config) {
    validate_client_tls_config(config.tls);
    const auto driver = config.driver;
    if (driver == DbDriver::kMariaDb && config.tls.mode == client_tls_mode::verify_identity) {
        std::error_code error;
        (void)asio::ip::make_address(config.host, error);
        if (error || !config.tls.server_name.empty()) {
            throw std::invalid_argument("MariaDB authenticated TLS requires a numeric host with an IP certificate; server_name is unsupported");
        }
    }
    if (driver != DbDriver::kMariaDb && driver != DbDriver::kPostgreSql) {
        throw std::invalid_argument("database driver must be selected");
    }
#ifndef RUVIA_ENABLE_MARIADB
    if (driver == DbDriver::kMariaDb) {
        throw std::invalid_argument("MariaDB support is not enabled");
    }
#endif
#ifndef RUVIA_ENABLE_POSTGRESQL
    if (driver == DbDriver::kPostgreSql) {
        throw std::invalid_argument("PostgreSQL support is not enabled");
    }
#endif

    ruvia::ensureConfigHost(config.host, "database host must not be empty", "database host is invalid",
        ruvia::kSeparatedPortHostRules);
    ruvia::ensureNonZeroPort(configuredDbPort(config), "database port must not be zero");
    ruvia::ensurePositiveOptionalDurations("configured database timeouts must be greater than zero",
        config.connectTimeout, config.readTimeout, config.writeTimeout, config.queryTimeout,
        config.acquireTimeout);
}

[[nodiscard]] inline ValidatedDbConfigView validatedDbConfig(const DbConfig& config) {
    validateDbConfig(config);
    return ValidatedDbConfigView(config);
}

ValidatedDbConfigView validatedDbConfig(DbConfig&&) = delete;
ValidatedDbConfigView validatedDbConfig(const DbConfig&&) = delete;

}  // namespace ruvia::detail
