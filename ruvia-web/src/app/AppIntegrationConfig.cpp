#include "app/AppConfigMutation.h"
#include "integration/NamedCapability.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "db/DbConfigStorage.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis/RedisConfigStorage.h"
#endif

namespace ruvia {
#ifdef RUVIA_ENABLE_DATABASE
App& App::database(DbRegistrationConfig config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot configure database while app is running", [&](detail::AppState& state) {
            auto* resource = detail::appResource();
            detail::validateCapabilityAlias(config.alias, "database alias must not be empty");
            detail::DbDefinition replacement{std::pmr::string(config.alias, resource),
                detail::DbConfigStorage(config.config, resource)};
            if (config.query_cache) {
                replacement.query_cache.emplace(*config.query_cache, resource);
            }
            for (auto& definition : state.databases) {
                if (std::string_view(definition.alias) == std::string_view(config.alias)) {
                    definition = std::move(replacement);
                    return;
                }
            }
            state.databases.push_back(std::move(replacement));
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
            detail::upsertNamedCapabilityDefinition(state.redis, config.alias, config.config,
                "redis alias must not be empty", detail::appResource());
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
            detail::upsertNamedCapabilityDefinition(state.httpClients, config.alias, config.config,
                "HTTP client alias must not be empty", detail::appResource());
        });
}

App& App::httpClient(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot configure HTTP client while app is running",
        [](detail::AppState& state) { state.httpClients.clear(); });
}
}  // namespace ruvia
