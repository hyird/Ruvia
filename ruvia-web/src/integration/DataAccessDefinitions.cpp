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

[[nodiscard]] DbConfig cloneDbConfig(const DbConfig& source, std::pmr::memory_resource* resource) {
    return DbConfig{
        .driver = source.driver,
        .host = std::pmr::string(source.host, resource),
        .port = source.port,
        .username = std::pmr::string(source.username, resource),
        .password = std::pmr::string(source.password, resource),
        .database = std::pmr::string(source.database, resource),
        .connectTimeout = source.connectTimeout,
        .readTimeout = source.readTimeout,
        .writeTimeout = source.writeTimeout,
        .queryTimeout = source.queryTimeout,
        .acquireTimeout = source.acquireTimeout,
    };
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

[[nodiscard]] RedisConfig cloneRedisConfig(const RedisConfig& source, std::pmr::memory_resource* resource) {
    return RedisConfig{
        .host = std::pmr::string(source.host, resource),
        .port = source.port,
        .username = std::pmr::string(source.username, resource),
        .password = std::pmr::string(source.password, resource),
        .database = source.database,
        .poolSizePerWorker = source.poolSizePerWorker,
        .connectTimeout = source.connectTimeout,
        .commandTimeout = source.commandTimeout,
        .acquireTimeout = source.acquireTimeout,
        .maxReplyBytes = source.maxReplyBytes,
        .maxArrayDepth = source.maxArrayDepth,
        .tcpNoDelay = source.tcpNoDelay,
        .keepAlive = source.keepAlive,
    };
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
