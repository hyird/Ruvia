#pragma once

#include <system_error>

#include <asio/ip/address.hpp>

#include "ruvia/web/db/db_types.h"

#include "client/client_tls_config_storage.h"
#include "util/config_validation.h"

namespace ruvia::detail {

class validated_db_config_view final {
public:
    [[nodiscard]] const db_config& get() const noexcept {
        return *config_;
    }

private:
    friend validated_db_config_view validated_db_config(const db_config& config);

    explicit validated_db_config_view(const db_config& config) noexcept
        : config_(&config) {}

    const db_config* config_;
};

[[nodiscard]] inline constexpr std::uint16_t default_db_port(db_driver driver) noexcept {
    switch (driver) {
        case db_driver::mariadb:
            return 3306;
        case db_driver::postgresql:
            return 5432;
        default:
            return 0;
    }
}

[[nodiscard]] inline std::uint16_t configured_db_port(const db_config& config) noexcept {
    return config.port_.value_or(default_db_port(config.driver_));
}

inline void validate_db_config(const db_config& config) {
    validate_client_tls_config(config.tls_);
    // Database TLS is always authenticated, and both drivers are configured
    // with key files only; refuse settings they would silently ignore.
    if (config.tls_.mode_ == client_tls_mode::skip_verification) {
        throw std::invalid_argument("database TLS requires peer verification");
    }
    if (!config.tls_.private_key_password_.empty()) {
        throw std::invalid_argument("database TLS does not support encrypted private keys");
    }
    const auto driver = config.driver_;
    if (driver == db_driver::mariadb && config.tls_.mode_ == client_tls_mode::verify_identity) {
        std::error_code error;
        (void)asio::ip::make_address(config.host_, error);
        if (error || config.tls_.server_name_.has_value()) {
            throw std::invalid_argument("MariaDB authenticated TLS requires a numeric host with an IP certificate; server_name is unsupported");
        }
    }
    if (driver != db_driver::mariadb && driver != db_driver::postgresql) {
        throw std::invalid_argument("database driver must be selected");
    }
#ifndef RUVIA_ENABLE_MARIADB
    if (driver == db_driver::mariadb) {
        throw std::invalid_argument("MariaDB support is not enabled");
    }
#endif
#ifndef RUVIA_ENABLE_POSTGRESQL
    if (driver == db_driver::postgresql) {
        throw std::invalid_argument("PostgreSQL support is not enabled");
    }
#endif

    detail::ensure_config_host(config.host_, "database host must not be empty", "database host is invalid");
    // Both drivers pass these to C APIs as NUL-terminated strings.
    for (const std::string_view value : {std::string_view{config.username_}, std::string_view{config.password_},
             std::string_view{config.database_}}) {
        if (value.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("database credentials must not contain NUL bytes");
        }
    }
    detail::ensure_non_zero_port(configured_db_port(config), "database port must not be zero");
    detail::ensure_positive_optional_durations("configured database timeouts must be greater than zero",
        config.connect_timeout_, config.read_timeout_, config.write_timeout_, config.query_timeout_,
        config.acquire_timeout_);
}

[[nodiscard]] inline validated_db_config_view validated_db_config(const db_config& config) {
    validate_db_config(config);
    return validated_db_config_view(config);
}

validated_db_config_view validated_db_config(db_config&&) = delete;
validated_db_config_view validated_db_config(const db_config&&) = delete;

}  // namespace ruvia::detail
