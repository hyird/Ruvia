#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ruvia/web/redis/redis_types.h"

#include "client/client_tls.h"
#include "integration/named_capability.h"
#include "server/config_validation.h"
#include "server/tcp_socket_policies.h"

namespace ruvia::detail {

inline void validate_redis_config(const redis_config& config) {
    validate_client_tls_config(config.tls_);
    validate_client_origin_host(config.host_, "redis host must not be empty", "redis host is invalid");
    detail::ensure_non_zero_port(config.port_.value(), "redis port must not be zero");
    detail::ensure_positive_size(config.pool_size_per_worker_, "redis pool size must be greater than zero");
    detail::ensure_positive_size(
        config.blocking_pool_size_per_worker_, "redis blocking pool size must be greater than zero");
    detail::ensure_positive_optional_durations("configured redis timeouts must be greater than zero",
        config.connect_timeout_, config.command_timeout_, config.acquire_timeout_);
    detail::ensure_positive_size(config.max_array_depth_, "redis max array depth must be greater than zero");
    detail::ensure_positive_optional_size(
        config.max_reply_bytes_, "configured redis reply byte limit must be greater than zero");
    validate_client_tcp_config(config.tcp_);
    detail::ensure_positive_optional_duration(
        config.subscription_ping_interval_, "redis subscription ping interval must be greater than zero");
}

// PMR-owned runtime form of the ordinary public redis_config value.
struct redis_config_storage final {
    redis_config_storage(const redis_config& source_value)
        : redis_config_storage(
              validated_config_tag_type{}, validate(source_value)) {}

    redis_config_storage(const redis_config_storage& source_value)
        : redis_config_storage(validated_config_tag_type{}, source_value) {}

    std::string host_;
    std::uint16_t port_;
    std::string username_;
    std::string password_;
    client_tls_config_storage tls_;
    std::uint32_t database_;
    std::size_t pool_size_per_worker_;
    std::size_t blocking_pool_size_per_worker_;
    std::optional<timeout_duration> connect_timeout_;
    std::optional<timeout_duration> command_timeout_;
    std::optional<timeout_duration> acquire_timeout_;
    std::optional<std::size_t> max_reply_bytes_;
    std::size_t max_array_depth_;
    client_tcp_config tcp_;
    std::optional<timeout_duration> subscription_ping_interval_;

private:
    struct validated_config_tag_type final {};

    [[nodiscard]] static const redis_config& validate(const redis_config& source_value) {
        validate_redis_config(source_value);
        return source_value;
    }

    [[nodiscard]] static std::uint16_t resolved_port(const redis_config& source) noexcept {
        return source.port_.value();
    }
    [[nodiscard]] static std::uint16_t resolved_port(const redis_config_storage& source) noexcept {
        return source.port_;
    }

    // Public configuration parses the TLS files every pool's context
    // installs; allocator-rebound copies share them.
    template <typename source_type>
    [[nodiscard]] static client_tls_config_storage tls_storage(
        const source_type& source_value) {
        if constexpr (std::same_as<source_type, redis_config>) {
            return {source_value.tls_, client_tls_files::parse};
        } else {
            return {source_value.tls_};
        }
    }

    // Public configuration validation and allocator-rebound copies enter the
    // same owning construction path; no intermediate string copies or views.
    template <typename source_type>
        requires(std::same_as<source_type, redis_config> ||
                    std::same_as<source_type, redis_config_storage>)
    redis_config_storage(
        validated_config_tag_type, const source_type& source_value)
        : host_(source_value.host_),
          port_(resolved_port(source_value)),
          username_(source_value.username_),
          password_(source_value.password_),
          tls_(tls_storage(source_value)),
          database_(source_value.database_),
          pool_size_per_worker_(source_value.pool_size_per_worker_),
          blocking_pool_size_per_worker_(source_value.blocking_pool_size_per_worker_),
          connect_timeout_(source_value.connect_timeout_),
          command_timeout_(source_value.command_timeout_),
          acquire_timeout_(source_value.acquire_timeout_),
          max_reply_bytes_(source_value.max_reply_bytes_),
          max_array_depth_(source_value.max_array_depth_),
          tcp_(source_value.tcp_),
          subscription_ping_interval_(source_value.subscription_ping_interval_) {}
};

using redis_definition_type = named_capability_definition<redis_config_storage>;

}  // namespace ruvia::detail
