#include "ruvia/web/detail/app/AppConfigMutation.h"
#include "ruvia/web/detail/integration/CapabilityAlias.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/detail/db/DbConfigStorage.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/detail/redis/RedisConfigStorage.h"
#endif

#include <memory_resource>
#include <utility>

namespace ruvia {
namespace {

template <typename Definition, typename Config, typename MakeDefinition>
void upsertDefinition(std::pmr::vector<Definition>& definitions, std::string_view alias,
    Config& config, MakeDefinition&& makeDefinition) {
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
App& App::database(DbRegistrationConfig config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot configure database while app is running", [&](detail::AppState& state) {
            detail::validateCapabilityAlias(config.alias, "database alias must not be empty");
            auto storedConfig = detail::DbConfigStorage(config.config, detail::appResource());
            upsertDefinition(state.databases, config.alias, storedConfig,
                [](std::string_view storedAlias, detail::DbConfigStorage&& definitionConfig) {
                    auto* resource = detail::appResource();
                    return detail::DbDefinition{
                        std::pmr::string(storedAlias, resource), std::move(definitionConfig)};
                });
        });
}

App& App::database(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot configure database while app is running",
        [](detail::AppState& state) { state.databases.clear(); });
}
#endif

#ifdef RUVIA_ENABLE_REDIS
App& App::redis(RedisRegistrationConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure redis while app is running",
        [&](detail::AppState& state) {
            detail::validateCapabilityAlias(config.alias, "redis alias must not be empty");
            auto storedConfig = detail::RedisConfigStorage(config.config, detail::appResource());
            upsertDefinition(state.redis, config.alias, storedConfig,
                [](std::string_view storedAlias, detail::RedisConfigStorage&& definitionConfig) {
                    auto* resource = detail::appResource();
                    return detail::RedisDefinition{
                        std::pmr::string(storedAlias, resource), std::move(definitionConfig)};
                });
        });
}

App& App::redis(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure redis while app is running",
        [](detail::AppState& state) { state.redis.clear(); });
}
#endif

App& App::httpClient(HttpClientRegistrationConfig config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot configure HTTP client while app is running", [&](detail::AppState& state) {
            detail::validateCapabilityAlias(config.alias, "HTTP client alias must not be empty");
            auto storedConfig =
                detail::HttpClientConfigStorage(config.config, detail::appResource());
            upsertDefinition(state.httpClients, config.alias, storedConfig,
                [](std::string_view storedAlias,
                    detail::HttpClientConfigStorage&& definitionConfig) {
                    auto* resource = detail::appResource();
                    return detail::HttpClientDefinition{
                        std::pmr::string(storedAlias, resource), std::move(definitionConfig)};
                });
        });
}

App& App::httpClient(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot configure HTTP client while app is running",
        [](detail::AppState& state) { state.httpClients.clear(); });
}
}  // namespace ruvia
