#pragma once

#include "ruvia/core/detail/config/ConfigValidation.h"
#include "ruvia/core/detail/io/TcpSocketOptions.h"
#include "ruvia/web/redis/RedisTypes.h"

namespace ruvia::detail {

inline void validateRedisConfig(const RedisConfig& config) {
    ensureConfigHost(config.host, "redis host must not be empty", "redis host is invalid",
        kSeparatedPortHostRules);
    ensureNonZeroPort(config.port, "redis port must not be zero");
    ensurePositiveSize(config.poolSizePerWorker, "redis pool size must be greater than zero");
    ensurePositiveSize(
        config.blockingPoolSizePerWorker, "redis blocking pool size must be greater than zero");
    ensurePositiveOptionalDurations("configured redis timeouts must be greater than zero",
        config.connectTimeout, config.commandTimeout, config.acquireTimeout);
    ensurePositiveSize(config.maxArrayDepth, "redis max array depth must be greater than zero");
    ensurePositiveOptionalSize(
        config.maxReplyBytes, "configured redis reply byte limit must be greater than zero");
    validateTcpNoDelayPolicy(config.tcpNoDelay);
    validateTcpKeepAlivePolicy(config.tcpKeepAlive);
}

}  // namespace ruvia::detail
