#include "ruvia/app/App.h"

#include <stdexcept>
#include <utility>

#include "AppConfigGuards.h"

namespace ruvia {

#ifdef RUVIA_ENABLE_MARIADB
App& App::useDb(DbConfig config) {
    return useDb("default", std::move(config));
}

App& App::useDb(std::string_view alias, DbConfig config) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot configure database while app is running");
    if (alias.empty()) {
        throw std::invalid_argument("database alias must not be empty");
    }

    for (auto& definition : databases_) {
        if (std::string_view(definition.alias) == alias) {
            definition.config = std::move(config);
            return *this;
        }
    }

    std::pmr::string storedAlias(alias, ProcessMemory::instance().upstreamResource());
    databases_.push_back(detail::DbDefinition{std::move(storedAlias), std::move(config)});
    return *this;
}
#endif

#ifdef RUVIA_ENABLE_REDIS
App& App::useRedis(RedisConfig config) {
    return useRedis("default", std::move(config));
}

App& App::useRedis(std::string_view alias, RedisConfig config) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot configure redis while app is running");
    if (alias.empty()) {
        throw std::invalid_argument("redis alias must not be empty");
    }

    for (auto& definition : redis_) {
        if (std::string_view(definition.alias) == alias) {
            definition.config = std::move(config);
            return *this;
        }
    }

    std::pmr::string storedAlias(alias, ProcessMemory::instance().upstreamResource());
    redis_.push_back(detail::RedisDefinition{std::move(storedAlias), std::move(config)});
    return *this;
}
#endif

#ifdef RUVIA_ENABLE_HTTP_CLIENT
App& App::useHttpClient(HttpClientConfig config) {
    return useHttpClient("default", std::move(config));
}

App& App::useHttpClient(std::string_view alias, HttpClientConfig config) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot configure http client while app is running");
    if (alias.empty()) {
        throw std::invalid_argument("http client alias must not be empty");
    }
    if (config.host.empty()) {
        throw std::invalid_argument("http client host must not be empty");
    }

    for (auto& definition : httpClients_) {
        if (std::string_view(definition.alias) == alias) {
            definition.config = std::move(config);
            return *this;
        }
    }

    auto* resource = ProcessMemory::instance().upstreamResource();
    httpClients_.push_back(detail::HttpClientDefinition{
        std::pmr::string(alias, resource),
        std::move(config)});
    return *this;
}
#endif

}  // namespace ruvia
