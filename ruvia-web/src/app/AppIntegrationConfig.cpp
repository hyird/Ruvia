#include "ruvia/web/detail/app/AppConfigMutation.h"
#include "ruvia/web/detail/integration/NamedCapability.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/detail/db/DbConfigStorage.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/detail/redis/RedisConfigStorage.h"
#endif

namespace ruvia {
#ifdef RUVIA_ENABLE_DATABASE
App& App::database(DbRegistrationConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure database while app is running", [&](detail::AppState& state) { detail::upsertNamedCapabilityDefinition(state.databases, config.alias, config.config, "database alias must not be empty", detail::appResource()); });
}

App& App::database(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure database while app is running", [](detail::AppState& state) { state.databases.clear(); });
}
#endif

#ifdef RUVIA_ENABLE_REDIS
App& App::redis(RedisRegistrationConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure redis while app is running", [&](detail::AppState& state) { detail::upsertNamedCapabilityDefinition(state.redis, config.alias, config.config, "redis alias must not be empty", detail::appResource()); });
}

App& App::redis(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure redis while app is running", [](detail::AppState& state) { state.redis.clear(); });
}
#endif

App& App::httpClient(HttpClientRegistrationConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure HTTP client while app is running", [&](detail::AppState& state) { detail::upsertNamedCapabilityDefinition(state.httpClients, config.alias, config.config, "HTTP client alias must not be empty", detail::appResource()); });
}

App& App::httpClient(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure HTTP client while app is running", [](detail::AppState& state) { state.httpClients.clear(); });
}
}  // namespace ruvia
