#pragma once

#include "ruvia/core/ConfigValidation.h"
#include "ruvia/core/TcpSocketOptions.h"
#include "ruvia/web/redis/RedisTypes.h"

namespace ruvia::detail {

inline void validateRedisConfig(const RedisConfig& config) {
    ruvia::ensureConfigHost(config.host, "redis host must not be empty", "redis host is invalid",
        ruvia::kSeparatedPortHostRules);
    ruvia::ensureNonZeroPort(config.port, "redis port must not be zero");
    ruvia::ensurePositiveSize(config.poolSizePerWorker, "redis pool size must be greater than zero");
    ruvia::ensurePositiveSize(
        config.blockingPoolSizePerWorker, "redis blocking pool size must be greater than zero");
    ruvia::ensurePositiveOptionalDurations("configured redis timeouts must be greater than zero",
        config.connectTimeout, config.commandTimeout, config.acquireTimeout);
    ruvia::ensurePositiveSize(config.maxArrayDepth, "redis max array depth must be greater than zero");
    ruvia::ensurePositiveOptionalSize(
        config.maxReplyBytes, "configured redis reply byte limit must be greater than zero");
    ruvia::validateTcpSocketPolicies(config.tcpNoDelay, config.tcpKeepAlive);
}

}  // namespace ruvia::detail
