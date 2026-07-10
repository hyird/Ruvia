#pragma once

#ifdef RUVIA_ENABLE_REDIS

#include "ruvia/redis/RedisTypes.h"
#include "ruvia/web/detail/app/ConfigValidation.h"

namespace ruvia::detail {

inline void validateRedisConfig(const RedisConfig& config) {
    ensureConfigHost(
        config.host,
        "redis host must not be empty",
        "redis host is invalid",
        kSeparatedPortHostRules);
    ensureNonZeroPort(config.port, "redis port must not be zero");
    ensurePositiveSize(config.poolSizePerWorker, "redis pool size must be greater than zero");
    ensureNonNegativeDurations(
        "redis timeouts must not be negative",
        config.connectTimeout,
        config.commandTimeout,
        config.acquireTimeout);
    ensurePositiveSize(config.maxArrayDepth, "redis max array depth must be greater than zero");
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_REDIS
