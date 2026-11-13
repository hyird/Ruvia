#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/web/db/db_types.h"

#include "client/client_tls.h"
#include "integration/named_capability.h"
#include "server/config_validation.h"

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
    return config.port_.has_value() ? config.port_->value() : default_db_port(config.driver_);
}

inline void validate_db_config(const db_config& config) {
    validate_client_tls_config(config.tls_);
    // Both drivers are configured with key files only; refuse a password they
    // would silently ignore.
    if (config.tls_.identity_.has_value() && !config.tls_.identity_->private_key_password_.empty()) {
        throw std::invalid_argument("database TLS does not support encrypted private keys");
    }
    const auto driver = config.driver_;
    if (driver == db_driver::mariadb && config.tls_.mode_ == client_tls_mode::verify_identity) {
        if (!is_client_ip_address(config.host_) || config.tls_.server_name_.has_value()) {
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

    validate_client_origin_host(config.host_, "database host must not be empty", "database host is invalid");
    // Both drivers pass these to C APIs as NUL-terminated strings.
    for (const std::string_view value : {std::string_view{config.username_}, std::string_view{config.password_},
             std::string_view{config.database_}}) {
        if (value.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("database credentials must not contain NUL bytes");
        }
    }
    detail::ensure_non_zero_port(configured_db_port(config), "database port must not be zero");
    detail::ensure_positive_size(config.pool_size_per_worker_, "database pool size must be greater than zero");
    detail::ensure_positive_optional_durations("configured database timeouts must be greater than zero",
        config.connect_timeout_, config.read_timeout_, config.write_timeout_, config.query_timeout_,
        config.acquire_timeout_, config.transaction_timeout_);
}

[[nodiscard]] inline validated_db_config_view validated_db_config(const db_config& config) {
    validate_db_config(config);
    return validated_db_config_view(config);
}

validated_db_config_view validated_db_config(db_config&&) = delete;
validated_db_config_view validated_db_config(const db_config&&) = delete;

#ifdef RUVIA_ENABLE_REDIS
struct db_cache_config_storage final {
    template <typename config_type>
    db_cache_config_storage(const config_type& source_value)
        : duration_(source_value.duration_),
          always_enabled_(source_value.always_enabled_),
          ignore_errors_(source_value.ignore_errors_),
          key_prefix_(source_value.key_prefix_) {
        if (duration_.count() <= 0 || key_prefix_.empty()) {
            throw std::invalid_argument("database cache requires a positive duration and nonempty namespace");
        }
    }
    std::chrono::milliseconds duration_;
    bool always_enabled_;
    bool ignore_errors_;
    std::string key_prefix_;
};
#endif

// Worker/app-owned copy of the public startup configuration. Retained runtime
// state is explicitly rebound to its owning PMR domain here.
struct db_config_storage final {
    db_config_storage(const db_config& source_value)
        : db_config_storage(validated_db_config(source_value)) {}

    db_config_storage(validated_db_config_view source_value)
        : db_config_storage(validated_config_tag_type{}, source_value.get()) {}

    db_config_storage(db_config_storage&&) noexcept = default;
    db_config_storage(const db_config_storage& source_value)
        : db_config_storage(validated_config_tag_type{}, source_value) {}

    db_driver driver_{db_driver::unspecified};
    std::string host_;
    std::uint16_t port_{0};
    std::string username_;
    std::string password_;
    client_tls_config_storage tls_;
    std::string database_;
    std::size_t pool_size_per_worker_;
    std::optional<timeout_duration> connect_timeout_;
    std::optional<timeout_duration> read_timeout_;
    std::optional<timeout_duration> write_timeout_;
    std::optional<timeout_duration> query_timeout_;
    std::optional<timeout_duration> acquire_timeout_;
    std::optional<timeout_duration> transaction_timeout_;
    // Whether a slot whose connection ended connects again on its next use.
    // Off for a pool bound to the sessions it opened at startup (the
    // migrator's): a statement on a lost session then fails rather than run
    // on a new one that lacks the old one's session-scoped state.
    bool reconnect_{true};

private:
    struct validated_config_tag_type final {};

    [[nodiscard]] static std::uint16_t resolved_port(const db_config& source) noexcept {
        return configured_db_port(source);
    }
    [[nodiscard]] static std::uint16_t resolved_port(const db_config_storage& source) noexcept {
        return source.port_;
    }
    [[nodiscard]] static bool resolved_reconnect(const db_config&) noexcept {
        return true;
    }
    [[nodiscard]] static bool resolved_reconnect(const db_config_storage& source) noexcept {
        return source.reconnect_;
    }
    [[nodiscard]] static client_tls_config_storage tls_storage(
        const db_config& source) {
        return {source.tls_, client_tls_files::keep_paths};
    }
    [[nodiscard]] static client_tls_config_storage tls_storage(
        const db_config_storage& source) {
        return {source.tls_};
    }

    template <typename source_type>
        requires(std::same_as<source_type, db_config> || std::same_as<source_type, db_config_storage>)
    db_config_storage(validated_config_tag_type, const source_type& source_value)
        : driver_(source_value.driver_),
          host_(source_value.host_),
          port_(resolved_port(source_value)),
          username_(source_value.username_),
          password_(source_value.password_),
          tls_(tls_storage(source_value)),
          database_(source_value.database_),
          pool_size_per_worker_(source_value.pool_size_per_worker_),
          connect_timeout_(source_value.connect_timeout_),
          read_timeout_(source_value.read_timeout_),
          write_timeout_(source_value.write_timeout_),
          query_timeout_(source_value.query_timeout_),
          acquire_timeout_(source_value.acquire_timeout_),
          transaction_timeout_(source_value.transaction_timeout_),
          reconnect_(resolved_reconnect(source_value)) {}
};

#ifdef RUVIA_ENABLE_REDIS
struct db_query_cache_registration_storage final {
    db_query_cache_registration_storage(const db_query_cache_registration& source_value)
        : redis_alias_(source_value.redis_alias_),
          policy_(source_value.policy_) {
        validate_capability_alias(redis_alias_, "database query cache Redis alias must not be empty");
    }
    std::string redis_alias_;
    db_cache_config_storage policy_;
};
#endif

struct db_definition final {
    using config_storage_type = db_config_storage;
    std::string alias_;
    db_config_storage config_;
#ifdef RUVIA_ENABLE_REDIS
    std::optional<db_query_cache_registration_storage> query_cache_{};
#endif
};

}  // namespace ruvia::detail
