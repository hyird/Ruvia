#pragma once

#include "ruvia/web/redis/Redis.h"

#include "ruvia/web/detail/redis/RedisRegistry.h"

#include <memory_resource>
#include <optional>
#include <string_view>
#include <vector>

namespace ruvia::detail {

Task<void> executeRedisPing(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource);

Task<RedisSetResult> executeRedisSet(RedisCommandExecutor executor,
    std::pmr::vector<std::pmr::string> args, RedisSetOptions options,
    std::pmr::memory_resource* resource);

Task<bool> executeRedisIntegerBool(RedisCommandExecutor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);

Task<std::pmr::vector<RedisKeyValue>> executeRedisKeyValueArray(RedisCommandExecutor executor,
    std::pmr::vector<std::pmr::string> args, std::string_view context,
    std::pmr::memory_resource* resource);

Task<std::pmr::vector<RedisScoredValue>> executeRedisScoredArray(RedisCommandExecutor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);

Task<std::optional<double>> executeRedisOptionalDouble(RedisCommandExecutor executor,
    std::pmr::vector<std::pmr::string> args, std::string_view context,
    std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
