#include "ruvia/web/detail/integration/DataAccessDefinitions.h"

#include <stdexcept>
#include <utility>

namespace ruvia::detail {

EventLoop requireEventLoop(EventLoop loop) {
    if (!loop.valid()) {
        throw std::invalid_argument("data access service requires a valid event loop");
    }
    return loop;
}

[[nodiscard]] DbConfigStorage cloneDbConfig(const DbConfig& source, std::pmr::memory_resource* resource) {
    return DbConfigStorage(source, resource);
}

[[nodiscard]] DbConfigStorage cloneDbConfig(const DbConfigStorage& source, std::pmr::memory_resource* resource) {
    return DbConfigStorage(source, resource);
}

std::pmr::vector<DbDefinition> makeDatabaseDefinitions(const DataAccessOptions& options, std::pmr::memory_resource* resource) {
    std::pmr::vector<DbDefinition> definitions(resource);
#ifdef RUVIA_ENABLE_DATABASE
    definitions.reserve(options.databases.size());
    for (const auto& database : options.databases) {
        definitions.push_back(DbDefinition{std::pmr::string(database.alias, resource), cloneDbConfig(database.config, resource)});
    }
#else
    (void)options;
#endif
    return definitions;
}

[[nodiscard]] RedisConfigStorage cloneRedisConfig(const RedisConfig& source, std::pmr::memory_resource* resource) {
    return RedisConfigStorage(source, resource);
}

[[nodiscard]] RedisConfigStorage cloneRedisConfig(const RedisConfigStorage& source, std::pmr::memory_resource* resource) {
    return RedisConfigStorage(source, resource);
}

std::pmr::vector<RedisDefinition> makeRedisDefinitions(const DataAccessOptions& options, std::pmr::memory_resource* resource) {
    std::pmr::vector<RedisDefinition> definitions(resource);
#ifdef RUVIA_ENABLE_REDIS
    definitions.reserve(options.redis.size());
    for (const auto& redis : options.redis) {
        definitions.push_back(RedisDefinition{std::pmr::string(redis.alias, resource), cloneRedisConfig(redis.config, resource)});
    }
#else
    (void)options;
#endif
    return definitions;
}

}  // namespace ruvia::detail
