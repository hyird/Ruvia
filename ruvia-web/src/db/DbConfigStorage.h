#pragma once

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/db/DbCache.h"
#include "ruvia/web/db/DbTypes.h"

#include "client/ClientTlsConfigStorage.h"
#include "db/DbConfigValidation.h"
#include "integration/NamedCapability.h"

namespace ruvia::detail {

struct DbCacheConfigStorage final {
    template <typename Config>
    DbCacheConfigStorage(const Config& source, std::pmr::memory_resource* resource)
        : duration(source.duration),
          alwaysEnabled(source.alwaysEnabled),
          ignoreErrors(source.ignoreErrors),
          nameSpace(source.nameSpace, resource) {
#ifndef RUVIA_ENABLE_REDIS
        throw std::invalid_argument("database query caching requires Redis support");
#endif
        if (duration.count() <= 0 || nameSpace.empty()) {
            throw std::invalid_argument("database cache requires a positive duration and nonempty namespace");
        }
    }
    std::chrono::milliseconds duration;
    bool alwaysEnabled;
    bool ignoreErrors;
    std::pmr::string nameSpace;
};

// Worker/app-owned copy of the public startup configuration. Public DbConfig
// deliberately uses ordinary value types; retained runtime state is rebound to
// its owning PMR domain here.
struct DbConfigStorage final {
    DbConfigStorage(const DbConfig& source, std::pmr::memory_resource* resource)
        : DbConfigStorage(validatedDbConfig(source), pmrResourceOrDefault(resource)) {}

    DbConfigStorage(ValidatedDbConfigView source, std::pmr::memory_resource* resource)
        : DbConfigStorage(ValidatedConfigTag{}, source.get(), pmrResourceOrDefault(resource)) {}

    DbConfigStorage(const DbConfigStorage& source, std::pmr::memory_resource* resource)
        : DbConfigStorage(ValidatedConfigTag{}, source, pmrResourceOrDefault(resource)) {}

    DbDriver driver{DbDriver::kUnspecified};
    std::pmr::string host;
    std::uint16_t port{0};
    std::pmr::string username;
    std::pmr::string password;
    client_tls_config_storage tls;
    std::pmr::string database;
    std::optional<std::chrono::milliseconds> connectTimeout;
    std::optional<std::chrono::milliseconds> readTimeout;
    std::optional<std::chrono::milliseconds> write_timeout;
    std::optional<std::chrono::milliseconds> queryTimeout;
    std::optional<std::chrono::milliseconds> acquireTimeout;

private:
    struct ValidatedConfigTag final {};

    DbConfigStorage(ValidatedConfigTag, const DbConfig& source, std::pmr::memory_resource* resource)
        : driver(source.driver),
          host(source.host, resource),
          port(source.port.value_or(defaultDbPort(source.driver))),
          username(source.username, resource),
          password(source.password, resource),
          tls(source.tls, resource),
          database(source.database, resource),
          connectTimeout(source.connectTimeout),
          readTimeout(source.readTimeout),
          write_timeout(source.write_timeout),
          queryTimeout(source.queryTimeout),
          acquireTimeout(source.acquireTimeout) {
    }

    DbConfigStorage(
        ValidatedConfigTag, const DbConfigStorage& source, std::pmr::memory_resource* resource)
        : driver(source.driver),
          host(source.host, resource),
          port(source.port),
          username(source.username, resource),
          password(source.password, resource),
          tls(source.tls, resource),
          database(source.database, resource),
          connectTimeout(source.connectTimeout),
          readTimeout(source.readTimeout),
          write_timeout(source.write_timeout),
          queryTimeout(source.queryTimeout),
          acquireTimeout(source.acquireTimeout) {
    }
};

struct db_query_cache_registration_storage final {
    db_query_cache_registration_storage(const db_query_cache_registration& source,
        std::pmr::memory_resource* resource)
        : redis_alias(source.redis_alias, resource),
          policy(source.policy, resource) {
        validateCapabilityAlias(redis_alias, "database query cache Redis alias must not be empty");
    }
    std::pmr::string redis_alias;
    DbCacheConfigStorage policy;
};

struct DbDefinition final {
    using ConfigStorage = DbConfigStorage;
    std::pmr::string alias;
    DbConfigStorage config;
    std::optional<db_query_cache_registration_storage> query_cache{};
};

}  // namespace ruvia::detail
