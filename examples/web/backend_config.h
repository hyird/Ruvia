#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/web/app.h"

#include "backend_tls.h"

namespace example {

inline void assign_setting(std::string& target, const environment& env, std::string_view name) {
    if (const auto value = env.get(name)) {
        target.assign(*value);
    }
}

#ifdef RUVIA_ENABLE_DATABASE
inline ruvia::db_config database_config(const environment& env, ruvia::db_driver driver) {
    ruvia::db_config config{.driver_ = driver};
    assign_setting(config.host_, env, "RUVIA_DB_HOST");
    assign_setting(config.username_, env, "RUVIA_DB_USER");
    assign_setting(config.password_, env, "RUVIA_DB_PASSWORD");
    assign_setting(config.database_, env, "RUVIA_DB_DATABASE");
    config.tls_ = backend_tls("RUVIA_DB", env);
    if (const auto port = env.get<std::uint16_t>("RUVIA_DB_PORT")) {
        config.port_ = *port;
    }
    if (const auto size = env.get<std::size_t>("RUVIA_DB_POOL_SIZE")) {
        config.pool_size_per_worker_ = *size;
    }
    config.connect_timeout_ = std::chrono::seconds(5);
    config.query_timeout_ = std::chrono::seconds(10);
    return config;
}
#endif

#ifdef RUVIA_ENABLE_REDIS
inline ruvia::redis_config redis_config(const environment& env, std::string_view prefix = "RUVIA_REDIS") {
    const std::string base(prefix);
    ruvia::redis_config config;
    assign_setting(config.host_, env, base + "_HOST");
    assign_setting(config.username_, env, base + "_USER");
    assign_setting(config.password_, env, base + "_PASSWORD");
    config.tls_ = backend_tls(prefix, env);
    if (const auto port = env.get<std::uint16_t>(base + "_PORT")) {
        config.port_ = *port;
    }
    if (const auto database = env.get<std::uint32_t>(base + "_DATABASE")) {
        config.database_ = *database;
    }
    if (const auto size = env.get<std::uint32_t>(base + "_POOL_SIZE_PER_WORKER")) {
        config.pool_size_per_worker_ = *size;
    }
    if (const auto size = env.get<std::uint32_t>(base + "_BLOCKING_POOL_SIZE_PER_WORKER")) {
        config.blocking_pool_size_per_worker_ = *size;
    }
    return config;
}
#endif

}  // namespace example
