#include "ruvia/web/detail/app/AppConfigMutation.h"
#include "ruvia/web/detail/integration/DataAccessDefinitions.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"

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

App& App::useHttpClient(HttpClientConfig config) {
    return useHttpClient("default", std::move(config));
}

App& App::useHttpClient(std::string_view alias, HttpClientConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure http client while app is running", [&](detail::AppState& state) {
        if (alias.empty()) {
            throw std::invalid_argument("http client alias must not be empty");
        }
        detail::validateHttpClientConfig(config);
        detail::HttpClientConfigStorage storedConfig(config, detail::appResource());
        upsertDefinition(state.httpClients, alias, storedConfig, [](std::string_view storedAlias, detail::HttpClientConfigStorage&& definitionConfig) {
            auto* resource = detail::appResource();
            return detail::HttpClientDefinition{std::pmr::string(storedAlias, resource), std::move(definitionConfig)};
        });
    });
}

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

        auto storedConfig = detail::cloneDbConfig(config, detail::appResource());
        upsertDefinition(state.databases, alias, storedConfig, [](std::string_view storedAlias, detail::DbConfigStorage&& definitionConfig) {
            auto* resource = detail::appResource();
            return detail::DbDefinition{std::pmr::string(storedAlias, resource), std::move(definitionConfig)};
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

        auto storedConfig = detail::cloneRedisConfig(config, detail::appResource());
        upsertDefinition(state.redis, alias, storedConfig, [](std::string_view storedAlias, detail::RedisConfigStorage&& definitionConfig) {
            auto* resource = detail::appResource();
            return detail::RedisDefinition{std::pmr::string(storedAlias, resource), std::move(definitionConfig)};
        });
    });
}
#endif
}  // namespace ruvia
