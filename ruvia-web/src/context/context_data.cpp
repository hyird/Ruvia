#include "ruvia/web/context.h"

#include "context/context_services.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db_handle.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_handle.h"
#endif

namespace ruvia {

#ifdef RUVIA_ENABLE_DATABASE
db_handle context::db() const {
    return services().client_registries().db(operation_scope_, capabilities_.stop_token());
}

db_handle context::db(std::string_view alias) const {
    return services().client_registries().db(alias, operation_scope_, capabilities_.stop_token());
}
#endif

#ifdef RUVIA_ENABLE_REDIS
redis_handle context::redis() const {
    return services().client_registries().redis(operation_scope_, capabilities_.stop_token());
}

redis_handle context::redis(std::string_view alias) const {
    return services().client_registries().redis(alias, operation_scope_, capabilities_.stop_token());
}
#endif

}  // namespace ruvia
