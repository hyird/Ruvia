#include "ruvia/web/Context.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/DbHandle.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/RedisHandle.h"
#endif

namespace ruvia {

#ifdef RUVIA_ENABLE_DATABASE
DbHandle Context::db() const {
    return clientRegistries_.db(operationScope_, capabilities_.stop_token());
}

DbHandle Context::db(std::string_view alias) const {
    return clientRegistries_.db(alias, operationScope_, capabilities_.stop_token());
}
#endif

#ifdef RUVIA_ENABLE_REDIS
RedisHandle Context::redis() const {
    return clientRegistries_.redis(operationScope_, capabilities_.stop_token());
}

RedisHandle Context::redis(std::string_view alias) const {
    return clientRegistries_.redis(alias, operationScope_, capabilities_.stop_token());
}
#endif

}  // namespace ruvia
