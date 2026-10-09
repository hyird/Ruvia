#pragma once

#include "ruvia/core/config_validation.h"
#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/web/redis/redis_types.h"

#include "client/client_tls_config_storage.h"

namespace ruvia::detail {

inline void validate_redis_config(const redis_config& config) {
    validate_client_tls_config(config.tls_);
    ruvia::ensure_config_host(config.host_, "redis host must not be empty", "redis host is invalid",
        ruvia::separated_port_host_rules);
    ruvia::ensure_non_zero_port(config.port_, "redis port must not be zero");
    ruvia::ensure_positive_size(config.pool_size_per_worker_, "redis pool size must be greater than zero");
    ruvia::ensure_positive_size(
        config.blocking_pool_size_per_worker_, "redis blocking pool size must be greater than zero");
    ruvia::ensure_positive_optional_durations("configured redis timeouts must be greater than zero",
        config.connect_timeout_, config.command_timeout_, config.acquire_timeout_);
    ruvia::ensure_positive_size(config.max_array_depth_, "redis max array depth must be greater than zero");
    ruvia::ensure_positive_optional_size(
        config.max_reply_bytes_, "configured redis reply byte limit must be greater than zero");
    ruvia::validate_tcp_socket_policies(config.tcp_no_delay_, config.tcp_keep_alive_);
}

}  // namespace ruvia::detail
