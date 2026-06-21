#pragma once

#include "ruvia/redis/Redis.h"

#include "../RedisInternal.h"

#include <memory_resource>
#include <optional>
#include <string_view>
#include <vector>

namespace ruvia::detail {

Task<void> executeRedisPing(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource);

Task<std::optional<std::pmr::string>> executeRedisSetWithOptions(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    bool get,
    std::pmr::memory_resource* resource);

Task<bool> executeRedisSetNx(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource);

Task<bool> executeRedisIntegerBool(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource);

Task<std::pmr::vector<RedisKeyValue>> executeRedisKeyValueArray(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::string_view context,
    std::pmr::memory_resource* resource);

Task<std::pmr::vector<RedisScoredValue>> executeRedisScoredArray(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource);

Task<std::optional<double>> executeRedisOptionalDouble(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::string_view context,
    std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
