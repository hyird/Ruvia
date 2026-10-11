#pragma once

#include "ruvia/web/redis/redis_types.h"

#include "client/client_tls_config_storage.h"
#include "util/config_validation.h"

namespace ruvia::detail {

inline void validate_redis_config(const redis_config& config) {
    validate_client_tls_config(config.tls_);
    detail::ensure_config_host(config.host_, "redis host must not be empty", "redis host is invalid",
        detail::separated_port_host_rules);
    detail::ensure_non_zero_port(config.port_, "redis port must not be zero");
    detail::ensure_positive_size(config.pool_size_per_worker_, "redis pool size must be greater than zero");
    detail::ensure_positive_size(
        config.blocking_pool_size_per_worker_, "redis blocking pool size must be greater than zero");
    detail::ensure_positive_optional_durations("configured redis timeouts must be greater than zero",
        config.connect_timeout_, config.command_timeout_, config.acquire_timeout_);
    detail::ensure_positive_size(config.max_array_depth_, "redis max array depth must be greater than zero");
    detail::ensure_positive_optional_size(
        config.max_reply_bytes_, "configured redis reply byte limit must be greater than zero");
    validate_client_tcp_config(config.tcp_);
}

}  // namespace ruvia::detail
