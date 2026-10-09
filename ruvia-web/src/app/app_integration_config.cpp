#include "app/app_config_mutation.h"
#include "integration/named_capability.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "db/db_config_storage.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis/redis_config_storage.h"
#endif

namespace ruvia {
#ifdef RUVIA_ENABLE_DATABASE
application& application::database(db_registration_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure database while app is running", [&](detail::app_state& state_value) {
            auto* resource = detail::app_resource();
            detail::validate_capability_alias(config.alias_, "database alias must not be empty");
            detail::db_definition replacement{std::pmr::string(config.alias_, resource),
                detail::db_config_storage(config.config_, resource)};
            if (config.query_cache_) {
                replacement.query_cache_.emplace(*config.query_cache_, resource);
            }
            for (auto& definition : state_value.databases_) {
                if (std::string_view(definition.alias_) == std::string_view(config.alias_)) {
                    definition = std::move(replacement);
                    return;
                }
            }
            state_value.databases_.push_back(std::move(replacement));
        });
}

application& application::database(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure database while app is running",
        [](detail::app_state& state_value) { state_value.databases_.clear(); });
}
#endif

#ifdef RUVIA_ENABLE_REDIS
application& application::redis(redis_registration_config config) {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure redis while app is running",
        [&](detail::app_state& state_value) {
            detail::upsert_named_capability_definition(state_value.redis_, config.alias_, config.config_,
                "redis alias must not be empty", detail::app_resource());
        });
}

application& application::redis(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure redis while app is running",
        [](detail::app_state& state_value) { state_value.redis_.clear(); });
}
#endif

application& application::get_http_client(http_client_registration_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure HTTP client while app is running", [&](detail::app_state& state_value) {
            detail::upsert_named_capability_definition(state_value.http_clients_, config.alias_, config.config_,
                "HTTP client alias must not be empty", detail::app_resource());
        });
}

application& application::get_http_client(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure HTTP client while app is running",
        [](detail::app_state& state_value) { state_value.http_clients_.clear(); });
}
}  // namespace ruvia
