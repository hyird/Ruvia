#include "ruvia/web/Context.h"

#include "context/ContextServices.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/DbHandle.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/RedisHandle.h"
#endif

namespace ruvia {

#ifdef RUVIA_ENABLE_DATABASE
DbHandle Context::db() const {
    return services().clientRegistries().db(operationScope_, capabilities_.stop_token());
}

DbHandle Context::db(std::string_view alias) const {
    return services().clientRegistries().db(alias, operationScope_, capabilities_.stop_token());
}
#endif

#ifdef RUVIA_ENABLE_REDIS
RedisHandle Context::redis() const {
    return services().clientRegistries().redis(operationScope_, capabilities_.stop_token());
}

RedisHandle Context::redis(std::string_view alias) const {
    return services().clientRegistries().redis(alias, operationScope_, capabilities_.stop_token());
}
#endif

}  // namespace ruvia
