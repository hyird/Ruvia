#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/redis/RedisConfigValidation.h"
#include "ruvia/web/redis/RedisTypes.h"

namespace ruvia::detail {

// PMR-owned runtime form of the ordinary public RedisConfig value.
struct RedisConfigStorage final {
    RedisConfigStorage(const RedisConfig& source, std::pmr::memory_resource* resource)
        : RedisConfigStorage(
              ValidatedConfigTag{}, validate(source), pmrResourceOrDefault(resource)) {}

    RedisConfigStorage(const RedisConfigStorage& source, std::pmr::memory_resource* resource)
        : RedisConfigStorage(ValidatedConfigTag{}, source, pmrResourceOrDefault(resource)) {}

    std::pmr::string host;
    std::uint16_t port{6379};
    std::pmr::string username;
    std::pmr::string password;
    std::uint32_t database{0};
    std::size_t poolSizePerWorker{4};
    std::size_t blockingPoolSizePerWorker{1};
    std::optional<std::chrono::milliseconds> connectTimeout;
    std::optional<std::chrono::milliseconds> commandTimeout;
    std::optional<std::chrono::milliseconds> acquireTimeout;
    std::optional<std::size_t> maxReplyBytes{64 * 1024 * 1024};
    std::size_t maxArrayDepth{64};
    TcpNoDelayPolicy tcpNoDelay{TcpNoDelayPolicy::kEnable};
    TcpKeepAlivePolicy tcpKeepAlive{TcpKeepAlivePolicy::kSystemDefault};

private:
    struct ValidatedConfigTag final {};

    [[nodiscard]] static const RedisConfig& validate(const RedisConfig& source) {
        validateRedisConfig(source);
        return source;
    }

    RedisConfigStorage(
        ValidatedConfigTag, const RedisConfig& source, std::pmr::memory_resource* resource)
        : host(source.host, resource),
          port(source.port),
          username(source.username, resource),
          password(source.password, resource),
          database(source.database),
          poolSizePerWorker(source.poolSizePerWorker),
          blockingPoolSizePerWorker(source.blockingPoolSizePerWorker),
          connectTimeout(source.connectTimeout),
          commandTimeout(source.commandTimeout),
          acquireTimeout(source.acquireTimeout),
          maxReplyBytes(source.maxReplyBytes),
          maxArrayDepth(source.maxArrayDepth),
          tcpNoDelay(source.tcpNoDelay),
          tcpKeepAlive(source.tcpKeepAlive) {}

    RedisConfigStorage(
        ValidatedConfigTag, const RedisConfigStorage& source, std::pmr::memory_resource* resource)
        : host(source.host, resource),
          port(source.port),
          username(source.username, resource),
          password(source.password, resource),
          database(source.database),
          poolSizePerWorker(source.poolSizePerWorker),
          blockingPoolSizePerWorker(source.blockingPoolSizePerWorker),
          connectTimeout(source.connectTimeout),
          commandTimeout(source.commandTimeout),
          acquireTimeout(source.acquireTimeout),
          maxReplyBytes(source.maxReplyBytes),
          maxArrayDepth(source.maxArrayDepth),
          tcpNoDelay(source.tcpNoDelay),
          tcpKeepAlive(source.tcpKeepAlive) {}
};

struct RedisDefinition final {
    std::pmr::string alias;
    RedisConfigStorage config;
};

}  // namespace ruvia::detail
