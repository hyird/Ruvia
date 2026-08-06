#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>

#include "ruvia/web/redis/RedisTypes.h"

namespace ruvia::detail {

// PMR-owned runtime form of the ordinary public RedisConfig value.
struct RedisConfigStorage final {
    RedisConfigStorage(const RedisConfig& source, std::pmr::memory_resource* resource)
        : host(source.host, resource),
          port(source.port),
          username(source.username, resource),
          password(source.password, resource),
          database(source.database),
          poolSizePerWorker(source.poolSizePerWorker),
          connectTimeout(source.connectTimeout),
          commandTimeout(source.commandTimeout),
          acquireTimeout(source.acquireTimeout),
          maxReplyBytes(source.maxReplyBytes),
          maxArrayDepth(source.maxArrayDepth),
          tcpNoDelay(source.tcpNoDelay),
          keepAlive(source.keepAlive) {}

    RedisConfigStorage(const RedisConfigStorage& source, std::pmr::memory_resource* resource)
        : host(source.host, resource),
          port(source.port),
          username(source.username, resource),
          password(source.password, resource),
          database(source.database),
          poolSizePerWorker(source.poolSizePerWorker),
          connectTimeout(source.connectTimeout),
          commandTimeout(source.commandTimeout),
          acquireTimeout(source.acquireTimeout),
          maxReplyBytes(source.maxReplyBytes),
          maxArrayDepth(source.maxArrayDepth),
          tcpNoDelay(source.tcpNoDelay),
          keepAlive(source.keepAlive) {}

    std::pmr::string host;
    std::uint16_t port{6379};
    std::pmr::string username;
    std::pmr::string password;
    std::uint32_t database{0};
    std::size_t poolSizePerWorker{4};
    std::optional<std::chrono::milliseconds> connectTimeout;
    std::optional<std::chrono::milliseconds> commandTimeout;
    std::optional<std::chrono::milliseconds> acquireTimeout;
    std::optional<std::size_t> maxReplyBytes{64 * 1024 * 1024};
    std::size_t maxArrayDepth{64};
    bool tcpNoDelay{true};
    bool keepAlive{false};
};

struct RedisDefinition final {
    std::pmr::string alias;
    RedisConfigStorage config;
};

}  // namespace ruvia::detail
