#pragma once

#include <memory_resource>
#include <vector>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/web/DataAccess.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbConfigStorage.h"
#include "ruvia/web/redis/RedisTypes.h"
#include "ruvia/web/detail/redis/RedisConfigStorage.h"

// Turning the user's DataAccessOptions into the definitions a worker-local
// registry is built from. Every string the worker will keep is copied into the
// worker's own memory resource here, so a definition outlives the options it
// came from and allocates nowhere else.

namespace ruvia::detail {

// Throws std::invalid_argument if the loop is not usable.
[[nodiscard]] EventLoop requireEventLoop(EventLoop loop);

// Public configuration uses ordinary value types. Normalize every retained
// string into the app/worker resource before it enters a long-lived definition.
[[nodiscard]] DbConfigStorage cloneDbConfig(const DbConfig& source, std::pmr::memory_resource* resource);
[[nodiscard]] DbConfigStorage cloneDbConfig(const DbConfigStorage& source, std::pmr::memory_resource* resource);

[[nodiscard]] RedisConfigStorage cloneRedisConfig(const RedisConfig& source, std::pmr::memory_resource* resource);
[[nodiscard]] RedisConfigStorage cloneRedisConfig(const RedisConfigStorage& source, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::vector<DbDefinition> makeDatabaseDefinitions(const DataAccessOptions& options, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::vector<RedisDefinition> makeRedisDefinitions(const DataAccessOptions& options, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
