#include "ruvia/web/detail/app/AppConfigMutation.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/detail/db/DbConfigValidation.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/detail/redis/RedisConfigValidation.h"
#endif

#include <memory_resource>
#include <stdexcept>
#include <utility>

namespace ruvia {
namespace {

template <typename Definition, typename Config, typename MakeDefinition>
void upsertDefinition(std::pmr::vector<Definition>& definitions, std::string_view alias, Config& config, MakeDefinition&& makeDefinition) {
    for (auto& definition : definitions) {
        if (std::string_view(definition.alias) == alias) {
            definition.config = std::move(config);
            return;
        }
    }

    definitions.push_back(std::forward<MakeDefinition>(makeDefinition)(alias, std::move(config)));
}

}  // namespace

#ifdef RUVIA_ENABLE_DATABASE
App& App::useDb(DbConfig config) {
    return useDb("default", std::move(config));
}

App& App::useDb(std::string_view alias, DbConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure database while app is running", [&](detail::AppState& state) {
        if (alias.empty()) {
            throw std::invalid_argument("database alias must not be empty");
        }
        detail::validateDbConfig(config);

        upsertDefinition(state.databases, alias, config, [](std::string_view storedAlias, DbConfig&& storedConfig) {
            auto* resource = detail::appResource();
            return detail::DbDefinition{std::pmr::string(storedAlias, resource), std::move(storedConfig)};
        });
    });
}
#endif

#ifdef RUVIA_ENABLE_REDIS
App& App::useRedis(RedisConfig config) {
    return useRedis("default", std::move(config));
}

App& App::useRedis(std::string_view alias, RedisConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure redis while app is running", [&](detail::AppState& state) {
        if (alias.empty()) {
            throw std::invalid_argument("redis alias must not be empty");
        }
        detail::validateRedisConfig(config);

        upsertDefinition(state.redis, alias, config, [](std::string_view storedAlias, RedisConfig&& storedConfig) {
            auto* resource = detail::appResource();
            return detail::RedisDefinition{std::pmr::string(storedAlias, resource), std::move(storedConfig)};
        });
    });
}
#endif
}  // namespace ruvia
