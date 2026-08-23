#pragma once

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"

namespace ruvia::detail {

// Worker/app-owned copy of the public startup configuration. Public DbConfig
// deliberately uses ordinary value types; retained runtime state is rebound to
// its owning PMR domain here.
struct DbConfigStorage final {
    DbConfigStorage(const DbConfig& source, std::pmr::memory_resource* resource)
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

    DbConfigStorage(const DbConfigStorage& source, std::pmr::memory_resource* resource)
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
};

struct DbDefinition final {
    std::pmr::string alias;
    DbConfigStorage config;
};

}  // namespace ruvia::detail
