#include "ruvia/web/detail/app/AppConfigMutation.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbConfigStorage.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/detail/redis/RedisConfigValidation.h"
#include "ruvia/web/detail/redis/RedisConfigStorage.h"
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
App& App::useDb(DbRegistrationOptions options) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure database while app is running", [&](detail::AppState& state) {
        if (options.alias.empty()) {
            throw std::invalid_argument("database alias must not be empty");
        }
        detail::validateDbConfig(options.config);

        auto storedConfig = detail::DbConfigStorage(options.config, detail::appResource());
        upsertDefinition(state.databases, options.alias, storedConfig, [](std::string_view storedAlias, detail::DbConfigStorage&& definitionConfig) {
            auto* resource = detail::appResource();
            return detail::DbDefinition{std::pmr::string(storedAlias, resource), std::move(definitionConfig)};
        });
    });
}
#endif

#ifdef RUVIA_ENABLE_REDIS
App& App::useRedis(RedisRegistrationOptions options) {
    return detail::mutateStoppedApp(*this, *state_, "cannot configure redis while app is running", [&](detail::AppState& state) {
        if (options.alias.empty()) {
            throw std::invalid_argument("redis alias must not be empty");
        }
        detail::validateRedisConfig(options.config);

        auto storedConfig = detail::RedisConfigStorage(options.config, detail::appResource());
        upsertDefinition(state.redis, options.alias, storedConfig, [](std::string_view storedAlias, detail::RedisConfigStorage&& definitionConfig) {
            auto* resource = detail::appResource();
            return detail::RedisDefinition{std::pmr::string(storedAlias, resource), std::move(definitionConfig)};
        });
    });
}
#endif
}  // namespace ruvia
