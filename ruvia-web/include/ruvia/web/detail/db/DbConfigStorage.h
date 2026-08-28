#pragma once

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/integration/NamedCapability.h"

namespace ruvia::detail {

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
    std::pmr::string database;
    std::optional<std::chrono::milliseconds> connectTimeout;
    std::optional<std::chrono::milliseconds> readTimeout;
    std::optional<std::chrono::milliseconds> writeTimeout;
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
          database(source.database, resource),
          connectTimeout(source.connectTimeout),
          readTimeout(source.readTimeout),
          writeTimeout(source.writeTimeout),
          queryTimeout(source.queryTimeout),
          acquireTimeout(source.acquireTimeout) {}

    DbConfigStorage(ValidatedConfigTag, const DbConfigStorage& source, std::pmr::memory_resource* resource)
        : driver(source.driver),
          host(source.host, resource),
          port(source.port),
          username(source.username, resource),
          password(source.password, resource),
          database(source.database, resource),
          connectTimeout(source.connectTimeout),
          readTimeout(source.readTimeout),
          writeTimeout(source.writeTimeout),
          queryTimeout(source.queryTimeout),
          acquireTimeout(source.acquireTimeout) {}
};

using DbDefinition = NamedCapabilityDefinition<DbConfigStorage>;

}  // namespace ruvia::detail
